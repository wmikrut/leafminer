#include <Arduino.h>
#include <climits>
#include "current.h"
#include "utils/log.h"
#include "screen/screen.h"

#if defined(ESP32)
  #include "freertos/FreeRTOS.h"
  #include "freertos/portmacro.h"
  static portMUX_TYPE g_hashes_mux = portMUX_INITIALIZER_UNLOCKED;
#elif defined(ESP8266)
  extern "C" {
    #include "ets_sys.h"   // ETS_INTR_LOCK / ETS_INTR_UNLOCK
  }
#endif

// 64-bit running total of all hashes since boot (used by *_by and get_* API)
static volatile uint64_t g_hashes_total = 0;

char TAG_CURRENT[] = "Current";

// Global variables
Job *current_job = nullptr;
Subscribe *current_subscribe = nullptr;
uint16_t current_job_is_valid = 0;
uint64_t current_job_processed = 0;
double current_difficulty = UINT_MAX;
double current_difficulty_highest = 0.0;
uint64_t current_block_found = 0;
uint64_t current_hash_accepted = 0;
uint64_t current_hash_rejected = 0;
uint32_t current_hashes = 0;
uint64_t current_hashes_time = 0;
double current_hashrate = 0;
uint64_t current_uptime = 0;
uint64_t current_last_hash = 0;

// Function prototypes
void deleteCurrentJob();
void deleteCurrentSubscribe();
void cleanupResources();
void handleException();

// Function implementations
bool current_hasJob()
{
    return current_job != nullptr;
}

void current_increment_processedJob()
{
    current_job_processed++;
}

void current_setJob(const Notification &notification)
{
    try
    {
        if (current_subscribe == nullptr)
        {
            l_error(TAG_CURRENT, "Subscribe object is null");
            return;
        }

        if (notification.clean_jobs)
        {
            current_job_is_valid = 0;
            if (current_job != nullptr)
            {
                l_debug(TAG_CURRENT, "Job: %s is cleaned and replaced with %s", current_job->job_id.c_str(), notification.job_id.c_str());
            }
        }

        deleteCurrentJob();

        current_job = new Job(notification, *current_subscribe, current_difficulty);
        current_job_is_valid = 1;
        current_increment_processedJob();
        l_info(TAG_CURRENT, "Job: %s ready to be mined", current_job->job_id.c_str());
    }
    catch (...)
    {
        handleException();
    }
}

void deleteCurrentJob()
{
    delete current_job;
    current_job = nullptr;
}

void current_resetSession()
{
    l_error(TAG_CURRENT, "Session reset");
    deleteCurrentSubscribe();
    current_job_is_valid = 0;
    deleteCurrentJob();
}

void deleteCurrentSubscribe()
{
    delete current_subscribe;
    current_subscribe = nullptr;
}

void current_setSubscribe(Subscribe *subscribe)
{
    try
    {
        l_info(TAG_CURRENT, "New session id: %s", subscribe->id.c_str());
        deleteCurrentSubscribe();
        current_subscribe = subscribe;
    }
    catch (...)
    {
        handleException();
    }
}

const char *current_getSessionId()
{
    return (current_subscribe != nullptr) ? current_subscribe->id.c_str() : nullptr;
}

void current_setDifficulty(double difficulty)
{
    try
    {
        l_info(TAG_CURRENT, "New difficulty: %.12f", difficulty);
        current_difficulty = difficulty;
    }
    catch (...)
    {
        handleException();
    }
}

const double current_getDifficulty()
{
    return current_difficulty;
}

void current_increment_block_found()
{
    current_block_found++;
    l_info(TAG_CURRENT, "Block found: %d", current_block_found);
}

const uint32_t current_get_block_found()
{
    return current_block_found;
}

const double current_get_hashrate()
{
    return current_hashrate;
}

void current_setHighestDifficulty(double difficulty)
{
    try
    {
        if (difficulty > current_difficulty_highest)
        {
            current_difficulty_highest = difficulty;
            l_info(TAG_CURRENT, "New highest hashed difficulty: %.12f", difficulty);
        }
    }
    catch (...)
    {
        handleException();
    }
}

const double current_getHighestDifficulty()
{
    return current_difficulty_highest;
}

void current_increment_hash_accepted()
{
    try
    {
        current_hash_accepted++;
        current_last_hash = millis();
        l_info(TAG_CURRENT, "Hash accepted: %d", current_hash_accepted);
    }
    catch (...)
    {
        handleException();
    }
}

const uint32_t current_get_hash_accepted()
{
    return current_hash_accepted;
}

void current_increment_hash_rejected()
{
    try
    {
        current_hash_rejected++;
        current_last_hash = millis();
        l_info(TAG_CURRENT, "Hash rejected: %d", current_hash_rejected);
    }
    catch (...)
    {
        handleException();
    }
}

void current_increment_hashes_by(uint32_t n)
{
    // Bump the rolling 1-second bucket that current_update_hashrate() reads
    // (existing logic expects current_hashes to be a simple counter).
    if (current_hashes_time == 0) {
        current_hashes_time = millis();
    }
    current_hashes += n;

    // Maintain a 64-bit lifetime total (thread/ISR-safe where applicable).
#if defined(ESP32)
    portENTER_CRITICAL(&g_hashes_mux);
    g_hashes_total += (uint64_t)n;
    portEXIT_CRITICAL(&g_hashes_mux);
#elif defined(ESP8266)
    ETS_INTR_LOCK();
    g_hashes_total += (uint64_t)n;
    ETS_INTR_UNLOCK();
#else
    g_hashes_total += (uint64_t)n;
#endif
}

// Keep legacy sites working; single step delegates to the batched adder.
void current_increment_hashes()
{
    current_increment_hashes_by(1);
}

uint64_t current_get_hashes_total(void)
{
    // Read without locking on ESP8266 (single core); on ESP32 your read is
    // typically fine without locking for UI/debug. If you want, you can
    // guard this with portENTER_CRITICAL just like the writer.
    return g_hashes_total;
}

const uint32_t current_get_hash_rejected()
{
    return current_hash_rejected;
}

// void current_increment_hashes()
// {
//     try
//     {
//         if (current_hashes_time == 0)
//         {
//             current_hashes_time = millis();
//         }
//         current_hashes++;
//     }
//     catch (...)
//     {
//         handleException();
//     }
// }

void current_update_hashrate()
{
    try
    {
        if (millis() - current_hashes_time > 1000)
        {
            current_hashrate = (current_hashes / ((millis() - current_hashes_time) / 1000.0)) / 1000.0; // KH/s
            l_debug(TAG_CURRENT, "Hashrate: %.2f kH/s", current_hashrate);
#if defined(HAS_LCD)
            screen_loop();
#endif
            current_hashes = 0;
            current_hashes_time = millis();
        }
    }
    catch (...)
    {
        handleException();
    }
}

void current_check_stale()
{
    try
    {
        if (millis() - current_last_hash > 200000)
        {
            l_error(TAG_CURRENT, "No hash received in the last 3 minutes. Restarting...");
            ESP.restart();
        }
    }
    catch (...)
    {
        handleException();
    }
}

void handleException()
{
    l_error(TAG_CURRENT, "Exception occurred. Cleaning up resources...");
    cleanupResources();
}

void cleanupResources()
{
    deleteCurrentJob();
    deleteCurrentSubscribe();
}

#if defined(ESP32)
#define CURRENT_STALE_TIMEOUT 50000
void currentTaskFunction(void *pvParameters)
{
    while (1)
    {
        current_check_stale();
        vTaskDelay(CURRENT_STALE_TIMEOUT / portTICK_PERIOD_MS);
    }
}
#endif
