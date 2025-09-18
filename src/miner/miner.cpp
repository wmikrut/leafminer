#include "miner/miner.h"
#include "utils/utils.h"
#include "leafminer.h"
#include "current.h"
#include "utils/log.h"
#include "network/network.h"
#if defined(HAS_LCD)
#include "screen/screen.h"
#endif

char TAG_MINER[] = "Miner";

// void miner(uint32_t core)
// {
//     double diff_hash = 0;
//     uint32_t winning_nonce = 0;
//     uint8_t hash[SHA256M_BLOCK_SIZE];

//     // Snapshot the job pointer to avoid races with the network thread.
//     const Job* job = current_job;
//     if (!job) {
//         l_error(TAG_MINER, "[%d] > No current job; aborting miner tick", core);
//         return;
//     }

//     //while (current_job_is_valid)
//     while (current_job_is_valid && job == current_job)  // stay on the same job
//     {
// #if defined(ESP8266)
//         ESP.wdtFeed();
// #endif
//         current_increment_hashes();

//         if (!current_job->pickaxe(core, hash, winning_nonce))
//         {
//             continue;
//         }

//         diff_hash = diff_from_target(hash);
//         if (diff_hash > current_getDifficulty())
//         {
//             l_debug(TAG_MINER, "[%d] > Hash %.12f > %.12f", core, diff_hash, current_getDifficulty());
//             break;
//         }
//         current_update_hashrate();
//     }

// #if defined(HAS_LCD)
//     screen_loop();
// #endif // HAS_LCD

//     //l_info(TAG_MINER, "[%d] > [%s] > 0x%.8x - diff %.12f", core, current_job->job_id.c_str(), winning_nonce, diff_hash);    
//     //network_send(current_job->job_id, current_job->extranonce2, current_job->ntime, winning_nonce);
//     // Re-check job snapshot still valid before using it
//     if (!job) {
//         l_error(TAG_MINER, "[%d] > Job vanished before submit/log; skipping", core);
//         return;
//     }
//     l_info(TAG_MINER, "[%d] > [%s] > 0x%.8x - diff %.12f",
//            core, job->job_id.c_str(), winning_nonce, diff_hash);
//     network_send(job->job_id, job->extranonce2, job->ntime, winning_nonce);

//     current_setHighestDifficulty(diff_hash);

//     //if (littleEndianCompare(hash, current_job->target.value, 32) < 0)
//     if (littleEndianCompare(hash, job->target.value, 32) < 0)
//     {
//         //l_info(TAG_MINER, "[%d] > Found block - 0x%.8x", core, current_job->block.nonce);
//         l_info(TAG_MINER, "[%d] > Found block - 0x%.8x", core, job->block.nonce);
//         current_increment_block_found();
//     }
// }

void miner(uint32_t core)
{
    // --- time-sliced mining to avoid starving networking ---
    const uint32_t SLICE_MS = 8;                 // good starting point on ESP8266
    const uint32_t t0 = millis();

    double   found_diff = 0.0;
    uint32_t found_nonce = 0;
    uint8_t  hash[SHA256M_BLOCK_SIZE];

    // Snapshot the job pointer once, avoid races; bail if missing.
    const Job* job = current_job;
    if (!job) {
        static uint32_t lastNoJobLogMs = 0;
        uint32_t now = millis();
        if (now - lastNoJobLogMs > 2000) {       // throttle this error
            l_error(TAG_MINER, "[%d] > No current job; aborting miner tick", core);
            lastNoJobLogMs = now;
        }
        return;
    }

    // Batch counters locally and apply at end (cheaper than atomic/globals each nonce).
    uint32_t local_hashes = 0;
    bool     share_found  = false;

    while ((millis() - t0) < SLICE_MS && current_job_is_valid && job == current_job)
    {
    #if defined(ESP8266)
        ESP.wdtFeed();
    #endif

        uint32_t winning_nonce = 0; // will be set by pickaxe on hit
        if (current_job->pickaxe(core, hash, winning_nonce)) {
            // We only compute difficulty & log when we actually have a candidate.
            const double diff_hash = diff_from_target(hash);
            if (diff_hash > current_getDifficulty()) {
                found_diff  = diff_hash;
                found_nonce = winning_nonce;
                share_found = true;
                break;  // submit after slice
            }
        }

        // Count one nonce worth of work for this loop (pickaxe() advances the nonce).
        local_hashes++;

        // Give the Wi-Fi stack a chance occasionally without tanking throughput.
        if ((local_hashes & 0x3FFF) == 0) { yield(); }
    }

    // Apply batched counters & a single hashrate update per slice.
    if (local_hashes) {
        current_increment_hashes_by(local_hashes);   // add this helper; fallback: loop or keep old call internally batched
        current_update_hashrate();
    }

#if defined(HAS_LCD)
    screen_loop();
#endif

    if (!share_found) return;

    // Re-check job snapshot still valid before using it.
    if (!job || job != current_job) return;

    l_info(TAG_MINER, "[%d] > [%s] > 0x%.8x - diff %.12f",
           core, job->job_id.c_str(), found_nonce, found_diff);
    network_send(job->job_id, job->extranonce2, job->ntime, found_nonce);

    current_setHighestDifficulty(found_diff);

    if (littleEndianCompare(hash, job->target.value, 32) < 0) {
        l_info(TAG_MINER, "[%d] > Found block - 0x%.8x", core, job->block.nonce);
        current_increment_block_found();
    }
}


#if defined(ESP32)
void mineTaskFunction(void *pvParameters)
{
    uint32_t core = (uint32_t)pvParameters;
    while (current_job_is_valid)
    {
        miner(core);
        vTaskDelay(33 / portTICK_PERIOD_MS); // Add a small delay to prevent tight loop
    }
}
#endif
