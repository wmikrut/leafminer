#include <vector>
#include <Arduino.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif // ESP8266
#include "model/configuration.h"
#include "network.h"
#include "utils/log.h"
#include "leafminer.h"
#include "current.h"
#include "utils/blink.h"

#define NETWORK_BUFFER_SIZE 2048
#define NETWORK_TIMEOUT 1000 * 60
#define NETWORK_DELAY 1222
#define NETWORK_WIFI_ATTEMPTS 2
#define NETWORK_STRATUM_ATTEMPTS 2
#define MAX_PAYLOAD_SIZE 384
#define MAX_PAYLOADS 10

WiFiClient client = WiFiClient();
char TAG_NETWORK[8] = "Network";
uint64_t id = 0;
uint64_t requestJobId = 0;
uint8_t isRequestingJob = 0;
uint32_t authorizeId = 0;
uint8_t isAuthorized = 0;
extern Configuration configuration;
char payloads[MAX_PAYLOADS][MAX_PAYLOAD_SIZE]; // Array of payloads
size_t payloads_count = 0;

// Back-pressure & correlation for submits
static volatile bool g_waitingSubmitResp = false;
static long long g_lastSubmitId = -1;
static uint32_t g_submitSentAtMs = 0;
static const uint32_t SUBMIT_TIMEOUT_MS = 10000;  // 10s safety
static uint32_t g_lastBackpressureLogMs = 0;

static String inputLine = "";
static uint32_t lastRxMs = millis();
static uint32_t lastIdleLogMs = 0;
const uint32_t QUIET_LOG_MS = 60000; // throttle idle logs

// Optional: quick telemetry to detect bursts of low-diff rejects
static uint16_t g_consecutiveLowDiff = 0;

static void restart_handshake(const char* why);

// helper: detect common "share accepted" replies from pools (NOMP, Miningcore, etc.)
// static bool is_share_accepted(const std::string& r) {
//     // Fast simple checks (avoid full JSON parse on ESP8266 unless you already do)
//     if (r.find("\"result\":true") != std::string::npos) return true;          // e.g., {"id":X,"result":true,"error":null}
//     if (r.find("\"status\":\"OK\"") != std::string::npos) return true;         // e.g., {"result":{"status":"OK"}}
//     if (r.find("\"accepted\":1") != std::string::npos) return true;            // some pools return counters
//     if (r.find("\"Status\":\"OK\"") != std::string::npos) return true;         // case variant
//     return false;
// }

/**
 * @brief Generates the next ID for the network.
 *
 * This function returns the next available ID for the network. If the current ID is equal to UINT64_MAX,
 * the function wraps around and returns 1. Otherwise, it increments the current ID by 1 and returns the result.
 *
 * @return The next ID for the network.
 */
uint64_t nextId()
{
    return (id == UINT64_MAX) ? 1 : ++id;
}

/**
 * Checks if the device is connected to the network.
 *
 * @note This function requires the configuration to be set.
 */
short isConnected()
{
    if (WiFi.status() == WL_CONNECTED && client.connected())
    {
        return 1;
    }

    uint16_t wifi_attemps = 0;

    // check if we are already connected to WiFi
    while (wifi_attemps < NETWORK_WIFI_ATTEMPTS)
    {
        l_info(TAG_NETWORK, "Connecting to %s...", configuration.wifi_ssid.c_str());
        WiFi.begin(configuration.wifi_ssid.c_str(), configuration.wifi_password.c_str());
        wifi_attemps++;
        delay(500);
        if (WiFi.waitForConnectResult() == WL_CONNECTED)
        {
            break;
        }
        delay(1500);
    }

    if (WiFi.waitForConnectResult() != WL_CONNECTED)
    {
        l_error(TAG_NETWORK, "Unable to connect to WiFi");
        return -1;
    }

    l_info(TAG_NETWORK, "Connected to WiFi");
    l_info(TAG_NETWORK, "IP address: %s", WiFi.localIP().toString().c_str());
    l_info(TAG_NETWORK, "MAC address: %s", WiFi.macAddress().c_str());

    uint16_t wifi_stratum = 0;

    // and we are connected to the host
    while (wifi_stratum < NETWORK_STRATUM_ATTEMPTS)
    {
        l_debug(TAG_NETWORK, "Connecting to host %s...", configuration.pool_url.c_str());
        client.connect(configuration.pool_url.c_str(), configuration.pool_port);
        delay(500);
        if (client.connected())
        {
            break;
        }
        wifi_stratum++;
        delay(1000);
    }

    if (!client.connected())
    {
        l_error(TAG_NETWORK, "Unable to connect to host");
        return -1;
    }

    return 1;
}

/**
 * Sends a request to the server with the specified payload.
 *
 * @param payload The payload to send to the server.
 */
// void request(const char *payload)
// {
//     client.print(payload);
//     l_info(TAG_NETWORK, ">>> %s", payload);
// }

void request(const char *payload)
{
    client.print(payload);
    size_t len = strlen(payload);
    if (len == 0 || payload[len - 1] != '\n') {
        client.print('\n'); // enforce LF-terminated JSON line
    }
    l_info(TAG_NETWORK, ">>> %s", payload);
}

/**
 * Authorizes the network connection by sending a request with the appropriate payload.
 */
void authorize()
{
    char payload[1024];
    uint64_t next_id = nextId();
    isAuthorized = 0;
    authorizeId = next_id;
    sprintf(payload, "{\"id\":%llu,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"%s\"]}\n", next_id, configuration.wallet_address.c_str(), configuration.pool_password.c_str());
    request(payload);
}

/**
 * Subscribes to the mining service.
 * Generates a payload with the subscription details and sends it as a request.
 */
void subscribe()
{
    char payload[1024];
    sprintf(payload, "{\"id\":%llu,\"method\":\"mining.subscribe\",\"params\":[\"LeafMiner/%s\", null]}\n", nextId(), _VERSION);
    request(payload);
}

/**
 * Suggests the mining difficulty for the network.
 * This function generates a payload string with the necessary data and sends it as a request.
 */
void difficulty()
{
    char payload[1024];
    sprintf(payload, "{\"id\":%llu,\"method\":\"mining.suggest_difficulty\",\"params\":[%f]}\n", nextId(), DIFFICULTY);
    request(payload);
}

/**
 * Determines the response type based on the provided JSON document.
 *
 * @param doc The JSON document to analyze.
 * @return The response type as a const char*.
 *         Possible values are "subscribe", the value of the "method" key,
 *         "mining.submit" if the "result" key is true, "mining.submit.fail" if the "result" key is false,
 *         or "unknown" if none of the above conditions are met.
 */
const char *responseType(cJSON *json)
{
    const cJSON *result = cJSON_GetObjectItem(json, "result");
    if (result != NULL && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0)
    {
        const cJSON *item0 = cJSON_GetArrayItem(result, 0);
        if (item0 != NULL && cJSON_IsArray(item0) && cJSON_GetArraySize(item0) > 0)
        {
            const cJSON *item00 = cJSON_GetArrayItem(item0, 0);
            if (item00 != NULL && cJSON_IsArray(item00) && cJSON_GetArraySize(item00) > 0)
            {
                return "subscribe";
            }
        }
    }
    else if (cJSON_HasObjectItem(json, "method"))
    {
        return cJSON_GetStringValue(cJSON_GetObjectItem(json, "method"));
    }
    else if (cJSON_HasObjectItem(json, "result"))
    {
        const cJSON *result = cJSON_GetObjectItem(json, "result");
        if (authorizeId == cJSON_GetNumberValue(cJSON_GetObjectItem(json, "id")))
        {
            return "authorized";
        }
        if (cJSON_IsTrue(result))
        {
            return "mining.submit";
        }
        else
        {
            // Map error codes on submit
            const cJSON *err = cJSON_GetObjectItem(json, "error");
            int code = (cJSON_IsArray(err) && cJSON_GetArraySize(err) > 0)
                    ? cJSON_GetNumberValue(cJSON_GetArrayItem(err, 0)) : 0;
            switch (code) {
                case 21: return "mining.submit.fail";                 // job not found
                case 23: return "mining.submit.difficulty_too_low";   // diff too low
                case 24: return "mining.unauthorized";                // worker lost auth
                default: return "mining.submit.fail";                 // any other error = fail
            }
        }
    }

    return "unknown";
}

static void clear_wait_if_matching_submit(cJSON* json) {
    const cJSON* id = cJSON_GetObjectItem(json, "id");
    if (cJSON_IsNumber(id) && (long long)id->valuedouble == g_lastSubmitId) {
        g_waitingSubmitResp = false;
        g_lastSubmitId = -1;
    }
}

/**
 * @brief Handles the response received from the network.
 *
 * This function parses the response JSON and performs different actions based on the response type.
 * The response type determines how the response data is processed and stored.
 *
 */
void response(std::string r)
{
    cJSON *json = cJSON_Parse(r.c_str());
    const char *type = responseType(json);
    l_info(TAG_NETWORK, "<<< [%s] %s", type, r.c_str());

    if (strcmp(type, "subscribe") == 0)
    {
        const cJSON *result = cJSON_GetObjectItem(json, "result");
        if (cJSON_IsArray(result) && cJSON_IsArray(cJSON_GetArrayItem(result, 0)) &&
            cJSON_IsArray(cJSON_GetArrayItem(cJSON_GetArrayItem(result, 0), 0)))
        {
            const cJSON *subscribeIdJson = cJSON_GetArrayItem(cJSON_GetArrayItem(cJSON_GetArrayItem(result, 0), 0), 1);
            const cJSON *extranonce1Json = cJSON_GetArrayItem(result, 1);
            const cJSON *extranonce2SizeJson = cJSON_GetArrayItem(result, 2);

            if (cJSON_IsString(subscribeIdJson) && cJSON_IsString(extranonce1Json) && cJSON_IsNumber(extranonce2SizeJson))
            {
                std::string subscribeId = subscribeIdJson->valuestring;
                std::string extranonce1 = extranonce1Json->valuestring;
                int extranonce2_size = extranonce2SizeJson->valueint;
                Subscribe *subscribe = new Subscribe(subscribeId, extranonce1, extranonce2_size);
                current_setSubscribe(subscribe);
            }
        }
    }
    else if (strcmp(type, "mining.notify") == 0)
    {

        // Don’t accept jobs before subscribe/session is set.
        if (current_getSessionId() == nullptr) {
            l_error(TAG_NETWORK, "Notify arrived before subscribe/session. Ignoring.");
            cJSON_Delete(json); r.clear(); return;
        }

        //cJSON *params = cJSON_GetObjectItem(json, "params");
        //std::string job_id = cJSON_GetArrayItem(params, 0)->valuestring;
        cJSON *params = cJSON_GetObjectItem(json, "params");
        if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) != 9) {
            l_error(TAG_NETWORK, "notify: params missing/invalid");
            cJSON_Delete(json); r.clear(); return;
        }
        cJSON *jid = cJSON_GetArrayItem(params, 0);
        if (!cJSON_IsString(jid) || !jid->valuestring) {
            l_error(TAG_NETWORK, "notify: job_id missing/invalid");
            cJSON_Delete(json); r.clear(); return;
        }
        std::string job_id = jid->valuestring;

        // fail fast check if job_id is the same as the current job
        if (current_hasJob() && strcmp(current_job->job_id.c_str(), job_id.c_str()) == 0)
        {
            l_error(TAG_NETWORK, "Job is the same as the current one");
            cJSON_Delete(json); r.clear(); 
            return;
        }        

        // Validate remaining fields and types
        cJSON *prev = cJSON_GetArrayItem(params, 1);
        cJSON *c1   = cJSON_GetArrayItem(params, 2);
        cJSON *c2   = cJSON_GetArrayItem(params, 3);
        cJSON *mb   = cJSON_GetArrayItem(params, 4);
        cJSON *ver  = cJSON_GetArrayItem(params, 5);
        cJSON *nb   = cJSON_GetArrayItem(params, 6);
        cJSON *nt   = cJSON_GetArrayItem(params, 7);
        cJSON *cln  = cJSON_GetArrayItem(params, 8);
        if (!cJSON_IsString(prev) || !cJSON_IsString(c1) || !cJSON_IsString(c2) ||
            !cJSON_IsArray(mb) || !cJSON_IsString(ver) || !cJSON_IsString(nb) ||
            !cJSON_IsString(nt) || (!cJSON_IsBool(cln) && !cJSON_IsNumber(cln))) {
            l_error(TAG_NETWORK, "notify: field types invalid");
            cJSON_Delete(json); r.clear(); return;
        }

        std::string prevhash = prev->valuestring;
        std::string coinb1   = c1->valuestring;
        std::string coinb2   = c2->valuestring;
        std::string version  = ver->valuestring;
        std::string nbits    = nb->valuestring;
        std::string ntime    = nt->valuestring;
        bool clean_jobs      = cJSON_IsBool(cln) ? cJSON_IsTrue(cln) : (cln->valueint == 1);

        // Reset stuck backpressure if a new clean_jobs notify arrives
        if (clean_jobs) {
        if (g_waitingSubmitResp) {
            l_info(TAG_NETWORK, "New clean job — dropping pending submit id=%lld", g_lastSubmitId);
            g_waitingSubmitResp = false;
            g_lastSubmitId = -1;
        }
    }

            
        std::vector<std::string> merkleBranchStrings;
        int merkleBranchSize = cJSON_GetArraySize(mb);
        for (int i = 0; i < merkleBranchSize; ++i) {
            cJSON *leaf = cJSON_GetArrayItem(mb, i);
            if (!cJSON_IsString(leaf) || !leaf->valuestring) {
                l_error(TAG_NETWORK, "notify: merkle branch item invalid");
                cJSON_Delete(json); r.clear(); return;
            }
            merkleBranchStrings.emplace_back(leaf->valuestring);
        }

        requestJobId = nextId();
            
        current_setJob(Notification(job_id, prevhash, coinb1, coinb2, merkleBranchStrings,
                                     version, nbits, ntime, clean_jobs));
        isRequestingJob = 0;
    }
    else if (strcmp(type, "mining.set_difficulty") == 0)
    {
        const cJSON *paramsArray = cJSON_GetObjectItem(json, "params");
        if (cJSON_IsArray(paramsArray) && cJSON_GetArraySize(paramsArray) == 1)
        {
            const cJSON *difficultyItem = cJSON_GetArrayItem(paramsArray, 0);
            if (cJSON_IsNumber(difficultyItem))
            {
                double diff = difficultyItem->valuedouble;
                current_setDifficulty(diff);
                l_debug(TAG_NETWORK, "Difficulty set to: %.10f", diff);
            }
        }
    }
    else if (strcmp(type, "authorized") == 0)
    {
        l_info(TAG_NETWORK, "Authorized");
        isAuthorized = 1;
    }
    else if (strcmp(type, "mining.submit") == 0)
    {               
        clear_wait_if_matching_submit(json);
        Blink::getInstance().blink(BLINK_SUBMIT);
        l_info(TAG_NETWORK, "Share accepted");
        g_consecutiveLowDiff = 0;
        current_increment_hash_accepted();
    }
    else if (strcmp(type, "mining.submit.difficulty_too_low") == 0)
    {
         clear_wait_if_matching_submit(json);
        l_error(TAG_NETWORK, "Share rejected due to low difficulty");
        current_increment_hash_rejected();
        if (++g_consecutiveLowDiff >= 3) {
            // brief RX focus to catch any pending set_difficulty/notify
            uint32_t until = millis() + 100;
            while (millis() < until) network_listen();
            g_consecutiveLowDiff = 0;
        }
    }    

    else if (strcmp(type, "mining.unauthorized") == 0) {
        clear_wait_if_matching_submit(json);
        l_error(TAG_NETWORK, "Worker unauthorized by pool. Re-subscribing and re-authorizing.");
        isAuthorized = 0;
        current_increment_hash_rejected();   // don't count it as accepted

        // Reset session so next getJob triggers a clean handshake
        g_waitingSubmitResp = false;
        g_lastSubmitId = -1;
        restart_handshake("unauthorized worker");

        // For ESP8266, also drop the socket so we start fresh
    #if defined(ESP8266)
        client.stop();
    #endif
        isRequestingJob = 0;

        if (isConnected() == 1) {
            // Proactively re-handshake now (optional but faster recovery)
            isRequestingJob = 0; 
            subscribe();
            authorize();
            difficulty();
        }

        return; // done with this response
    }
    else if (strcmp(type, "mining.submit.fail") == 0)
    {
        clear_wait_if_matching_submit(json);
        l_error(TAG_NETWORK, "Share rejected");

        // prevent the current from requesting a new job, being old responses
        if ((uint64_t)cJSON_GetObjectItem(json, "id")->valueint < requestJobId)
        {
            l_error(TAG_NETWORK, "Late responses, skip them");
        }
        else
        {
            current_job_is_valid = 0;
#if defined(ESP32)
            if (current_job_next != nullptr)
            {
                current_job = current_job_next;
                current_job_next = nullptr;
                current_job_is_valid = 1;
                l_debug(TAG_NETWORK, "Job (next): %s ready to be mined", current_job->job_id.c_str());
                current_increment_processedJob();
            }
#endif
            current_increment_hash_rejected();
        }
    }
    else
    {
        l_error(TAG_NETWORK, "Unknown response type: %s", type);
    }
    cJSON_Delete(json);
    r.clear();
}

short network_getJob()
{
    if (current_job_is_valid == 1)
    {
        l_info(TAG_NETWORK, "Already has a job and don't need a new one");
        return 0;
    }

    if (isRequestingJob == 1)
    {
        l_info(TAG_NETWORK, "Already requesting a job");
        return 0;
    }

    isRequestingJob = 1;

    if (isConnected() == -1)
    {
        g_waitingSubmitResp = false;
        g_lastSubmitId = -1;
        current_resetSession();        
        return -1;
    }

    if (current_getSessionId() == nullptr)
    {
        subscribe();
        authorize();
        difficulty();
    }

    return 1;
}

void enqueue(const char *payload)
{
    if (payloads_count < MAX_PAYLOADS)
    {
        strncpy(payloads[payloads_count], payload, MAX_PAYLOAD_SIZE - 1);
        payloads_count++;
        l_debug(TAG_NETWORK, "Payload queued: %s", payload);
    }
    else
    {
        l_error(TAG_NETWORK, "Payload queue is full");
    }
}

// void network_send(const std::string &job_id, const std::string &extranonce2, const std::string &ntime, const uint32_t &nonce)
// {
//     char payload[MAX_PAYLOAD_SIZE];
//     snprintf(payload, sizeof(payload), "{\"id\":%llu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%08x\"]}\n", nextId(), configuration.wallet_address.c_str(), job_id.c_str(), extranonce2.c_str(), ntime.c_str(), nonce);
// #if defined(ESP8266)
//     request(payload);
//     network_listen();
// #else
//     enqueue(payload);
// #endif
// }

void network_send(const std::string &job_id, const std::string &extranonce2, const std::string &ntime, const uint32_t &nonce)
{
#if defined(ESP8266)
    // Back-pressure: never queue a new submit until we got the reply to the last one
    if (g_waitingSubmitResp) {
        uint32_t now = millis();
        if (now - g_lastBackpressureLogMs > 1000) {
            // Give RX a chance and skip this tick
            network_listen();
            l_debug(TAG_NETWORK, "Backpressure: awaiting submit id=%lld", g_lastSubmitId);
            g_lastBackpressureLogMs = now;
        }
        return;
    }

    char payload[MAX_PAYLOAD_SIZE];
    uint64_t submitId = nextId();  // capture id so we can correlate the reply
    snprintf(payload, sizeof(payload),
             "{\"id\":%llu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%08x\"]}\n",
             submitId, configuration.wallet_address.c_str(), job_id.c_str(),
             extranonce2.c_str(), ntime.c_str(), nonce);    

    request(payload);
    // Immediately pump RX so we don’t fall behind
    network_listen();
    g_waitingSubmitResp = true;
    g_lastSubmitId      = (long long)submitId;
    g_submitSentAtMs    = millis();
#else
    // ESP32 path keeps the queue; can be left as-is or similarly guarded
    char payload[MAX_PAYLOAD_SIZE];
    uint64_t submitId = nextId();
    snprintf(payload, sizeof(payload),
             "{\"id\":%llu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%08x\"]}\n",
             submitId, configuration.wallet_address.c_str(), job_id.c_str(),
             extranonce2.c_str(), ntime.c_str(), nonce);
    enqueue(payload);
#endif
}

static void restart_handshake(const char* why) {
    l_error(TAG_NETWORK, "Restarting handshake: %s", why ? why : "unknown");

    // Kill any pending submit / backpressure
    g_waitingSubmitResp = false;
    g_lastSubmitId      = -1;
    g_submitSentAtMs    = 0;

    // Nuke session & job
    current_resetSession();  // clears subscribe & job state (your current.cpp already does this)

    // Drop the socket so client.connected() won't lie
#if defined(ESP8266)
    client.stop();
#else
    client.stop();
#endif

    // If we don't have a session, reconnect & handshake now
    if (current_getSessionId() == nullptr) {
        if (isConnected() == 1) {           // ensures WiFi + TCP connected (or reconnects)
            subscribe();
            authorize();
            difficulty();
            // Reset RX idle timer so we don't instantly trigger another restart
            lastRxMs = millis();
            // Also clear any partial line from previous socket
            inputLine = "";
        }
    }
}

void network_listen()
{    
    if (isConnected() == -1) {
        g_waitingSubmitResp = false;
        g_lastSubmitId = -1;
        current_resetSession();
        return;
    }

    // In network_listen() or your main loop watchdog:
    if ((millis() - lastRxMs) > 60000) { // 60s of silence
        l_error(TAG_NETWORK, "RX silent for 60s (waitingSubmit=%d id=%lld) — reconnecting", g_waitingSubmitResp, g_lastSubmitId);
        // close socket, reset session, resubscribe/authorize
        g_waitingSubmitResp = false;
        g_lastSubmitId = -1;
        restart_handshake("RX silent >60s");
    }

    // if (g_waitingSubmitResp && millis() - g_submitSentAt > 10000) { 
    //     l_error(TAG_NETWORK, "Timeout waiting for submit id=%lld, clearing backpressure", g_lastSubmitId);
    //     g_waitingSubmitResp = false;
    //     g_lastSubmitId = -1;
    // }
    // Safety: never wait forever on a lost submit response
    if (g_waitingSubmitResp) {
        uint32_t now = millis();
        uint32_t elapsed = (now - g_submitSentAtMs);
        if (elapsed > SUBMIT_TIMEOUT_MS) {
            l_error(TAG_NETWORK, "Submit timeout: id=%lld after %u ms — clearing backpressure", g_lastSubmitId, elapsed);
            g_waitingSubmitResp = false;
            g_lastSubmitId = -1;
            // Optional: bump a metric or LED pulse to make it visible
            restart_handshake("submit reply timeout");
        }
    }

    bool gotData = false;

    // Drain everything that’s ready without blocking the hasher
    while (client.available()) {
        char c = client.read();
        gotData = true;

        if (c == '\n') {
            if (inputLine.length() > 0) {
                l_debug(TAG_NETWORK, "<<< len: %d", inputLine.length());
                response(inputLine.c_str());
                inputLine = "";
            }
        } else if (c != '\r') {
            inputLine += c;
        }
        lastRxMs = millis();
    }

    // Don’t spam logs during normal quiet periods; only note prolonged silence
    if (!gotData) {
        uint32_t now = millis();
        if (now - lastRxMs > QUIET_LOG_MS && now - lastIdleLogMs > QUIET_LOG_MS) {
            l_debug(TAG_NETWORK, "Idle for %lus, still connected — continuing listen loop",
                    (now - lastRxMs) / 1000);
            lastIdleLogMs = now;
        }
    }

    // Keep Wi-Fi stack fed
    yield();    
}

void network_submit(const char *payload)
{
    if (isConnected() == -1)
    {
        g_waitingSubmitResp = false;
        g_lastSubmitId = -1;
        current_resetSession();
        return; // Handle connection failure
    }

    request(payload);

    // Remove the submitted payload from the array
    for (size_t i = 0; i < payloads_count; ++i)
    {
        if (strcmp(payloads[i], payload) == 0)
        {
            // Shift remaining payloads
            for (size_t j = i; j < payloads_count - 1; ++j)
            {
                strcpy(payloads[j], payloads[j + 1]);
            }
            payloads_count--;
            break;
        }
    }
}

void network_submit_all()
{
    for (size_t i = 0; i < payloads_count; ++i)
    {
        network_submit(payloads[i]);
    }
}

#if defined(ESP32)
#define NETWORK_TASK_TIMEOUT 100
void networkTaskFunction(void *pvParameters)
{
    while (1)
    {
        network_submit_all();
        network_listen();
        vTaskDelay(NETWORK_TASK_TIMEOUT / portTICK_PERIOD_MS);
    }
}
#endif