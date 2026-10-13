#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <nvs.h>
//#include "ShaTests/nerdSHA256.h"
#include "ShaTests/nerdSHA256plus.h"
#include "stratum.h"
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "timeconst.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include <mutex>
#include <list>
#include <map>
#include "mbedtls/sha256.h"
#include "i2c_master.h"
#include "wManager.h"

//10 Jobs per second
#define NONCE_PER_JOB_SW 4096
#define NONCE_PER_JOB_HW 16*1024

//#define I2C_SLAVE

//#define SHA256_VALIDATE
//#define RANDOM_NONCE
#define RANDOM_NONCE_MASK 0xFFFFC000

#ifdef HARDWARE_SHA265
#include <sha/sha_dma.h>
#include <hal/sha_hal.h>
#include <hal/sha_ll.h>

#if defined(CONFIG_IDF_TARGET_ESP32)
#include <sha/sha_parallel_engine.h>
#endif

#endif

nvs_handle_t stat_handle;

uint32_t templates = 0;
uint32_t hashes = 0;
uint32_t Mhashes = 0;
uint32_t totalKHashes = 0;
uint32_t elapsedKHs = 0;
uint64_t upTime = 0;

volatile uint32_t shares; // increase if blockhash has 32 bits of zeroes
volatile uint32_t valids; // increased if blockhash <= target

// Local live statistics (not persisted; reset on reboot)
static volatile uint32_t poolAccepted = 0;
static volatile uint32_t poolRejected = 0;
static volatile float liveHashrateKHs = 0.0f;
static volatile float avg10HashrateKHs = 0.0f;
static volatile float avg60HashrateKHs = 0.0f;
static WebServer statsServer(80);
static bool statsServerStarted = false;

// Track best diff
double best_diff = 0.0;

// Variables to hold data from custom textboxes
//Track mining stats in non volatile memory
extern TSettings Settings;

IPAddress serverIP(1, 1, 1, 1); //Temporally save poolIPaddres

//Global work data
static WiFiClient client;
static miner_data mMiner; //Global miner data (Create a miner class TODO)
mining_subscribe mWorker;
mining_job mJob;
monitor_data mMonitor;
static bool volatile isMinerSuscribed = false;
unsigned long mLastTXtoPool = millis();

int saveIntervals[7] = {5 * 60, 15 * 60, 30 * 60, 1 * 3600, 3 * 3600, 6 * 3600, 12 * 3600};
int saveIntervalsSize = sizeof(saveIntervals)/sizeof(saveIntervals[0]);
int currentIntervalIndex = 0;

bool checkPoolConnection(void) {

  if (client.connected()) {
    return true;
  }

  isMinerSuscribed = false;

  Serial.println("Client not connected, trying to connect...");

  //Resolve first time pool DNS and save IP
  if(serverIP == IPAddress(1,1,1,1)) {
    WiFi.hostByName(Settings.PoolAddress.c_str(), serverIP);
    Serial.printf("Resolved DNS and save ip (first time) got: %s\n", serverIP.toString());
  }

  //Try connecting pool IP
  if (!client.connect(serverIP, Settings.PoolPort)) {
    Serial.println("Imposible to connect to : " + Settings.PoolAddress);
    WiFi.hostByName(Settings.PoolAddress.c_str(), serverIP);
    Serial.printf("Resolved DNS got: %s\n", serverIP.toString());
    return false;
  }

  return true;
}

//Implements a socketKeepAlive function and
//checks if pool is not sending any data to reconnect again.
//Even connection could be alive, pool could stop sending new job NOTIFY
unsigned long mStart0Hashrate = 0;
bool checkPoolInactivity(unsigned int keepAliveTime, unsigned long inactivityTime){

    unsigned long currentKHashes = (Mhashes*1000) + hashes/1000;
    unsigned long elapsedKHs = currentKHashes - totalKHashes;

    uint32_t time_now = millis();

    // If no shares sent to pool
    // send something to pool to hold socket oppened
    if (time_now < mLastTXtoPool) //32bit wrap
      mLastTXtoPool = time_now;
    if ( time_now > mLastTXtoPool + keepAliveTime)
    {
      mLastTXtoPool = time_now;
      Serial.println("  Sending  : KeepAlive suggest_difficulty");
      //if (client.print("{}\n") == 0) {
      tx_suggest_difficulty(client, DEFAULT_DIFFICULTY);
      /*if(tx_suggest_difficulty(client, DEFAULT_DIFFICULTY)){
        Serial.println("  Sending keepAlive to pool -> Detected client disconnected");
        return true;
      }*/
    }

    if(elapsedKHs == 0){
      //Check if hashrate is 0 during inactivityTIme
      if(mStart0Hashrate == 0) mStart0Hashrate  = time_now;
      if((time_now-mStart0Hashrate) > inactivityTime) { mStart0Hashrate=0; return true;}
      return false;
    }

  mStart0Hashrate = 0;
  return false;
}

struct JobRequest
{
  uint32_t id;
  uint32_t nonce_start;
  uint32_t nonce_count;
  double difficulty;
  uint8_t sha_buffer[128];
  uint32_t midstate[8];
  uint32_t bake[16];
};

struct JobResult
{
  uint32_t id;
  uint32_t nonce;
  uint32_t nonce_count;
  double difficulty;
  uint8_t hash[32];
};

static std::mutex s_job_mutex;
std::list<std::shared_ptr<JobRequest>> s_job_request_list_sw;
#ifdef HARDWARE_SHA265
std::list<std::shared_ptr<JobRequest>> s_job_request_list_hw;
#endif
std::list<std::shared_ptr<JobResult>> s_job_result_list;
static volatile uint8_t s_working_current_job_id = 0xFF;

static void JobPush(std::list<std::shared_ptr<JobRequest>> &job_list,  uint32_t id, uint32_t nonce_start, uint32_t nonce_count, double difficulty,
                    const uint8_t* sha_buffer, const uint32_t* midstate, const uint32_t* bake)
{
  std::shared_ptr<JobRequest> job = std::make_shared<JobRequest>();
  job->id = id;
  job->nonce_start = nonce_start;
  job->nonce_count = nonce_count;
  job->difficulty = difficulty;
  memcpy(job->sha_buffer, sha_buffer, sizeof(job->sha_buffer));
  memcpy(job->midstate, midstate, sizeof(job->midstate));
  memcpy(job->bake, bake, sizeof(job->bake));
  job_list.push_back(job);
}

struct Submition
{
  double diff;
  bool is32bit;
  bool isValid;
};

static void MiningJobStop(uint32_t &job_pool, std::map<uint32_t, std::shared_ptr<Submition>> & submition_map)
{
  {
    std::lock_guard<std::mutex> lock(s_job_mutex);
    s_job_result_list.clear();
    s_job_request_list_sw.clear();
    #ifdef HARDWARE_SHA265
    s_job_request_list_hw.clear();
    #endif
  }
  s_working_current_job_id = 0xFF;
  job_pool = 0xFFFFFFFF;
  submition_map.clear();
}

#ifdef RANDOM_NONCE
uint64_t s_random_state = 1;
static uint32_t RandomGet()
{
    s_random_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = s_random_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#endif

void runStratumWorker(void *name) {

// TEST: https://bitcoin.stackexchange.com/questions/22929/full-example-data-for-scrypt-stratum-client

  Serial.println("");
  Serial.printf("\n[WORKER] Started. Running %s on core %d\n", (char *)name, xPortGetCoreID());

  #ifdef DEBUG_MEMORY
  Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  #endif

  std::map<uint32_t, std::shared_ptr<Submition>> s_submition_map;

#ifdef I2C_SLAVE
  std::vector<uint8_t> i2c_slave_vector;

  //scan for i2c slaves
  if (i2c_master_start() == 0)
    i2c_slave_vector = i2c_master_scan(0x0, 0x80);
  Serial.printf("Found %d slave workers\n", i2c_slave_vector.size());
  if (!i2c_slave_vector.empty())
  {
    Serial.print("  Workers: ");
    for (size_t n = 0; n < i2c_slave_vector.size(); ++n)
      Serial.printf("0x%02X,", (uint32_t)i2c_slave_vector[n]);
    Serial.println("");
  }
#endif

  // connect to pool
  double currentPoolDifficulty = DEFAULT_DIFFICULTY;
  uint32_t nonce_pool = 0;
  uint32_t job_pool = 0xFFFFFFFF;
  uint32_t last_job_time = millis();

  while(true) {

    if(WiFi.status() != WL_CONNECTED){
      // WiFi is disconnected, so reconnect now
      mMonitor.NerdStatus = NM_Connecting;
      MiningJobStop(job_pool, s_submition_map);
      WiFi.reconnect();
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    if(!checkPoolConnection()){
      //If server is not reachable add random delay for connection retries
      //Generate value between 1 and 60 secs
      MiningJobStop(job_pool, s_submition_map);
      vTaskDelay(((1 + rand() % 60) * 1000) / portTICK_PERIOD_MS);
      continue;
    }

    if(!isMinerSuscribed)
    {
      //Stop miner current jobs
      mWorker = init_mining_subscribe();

      // STEP 1: Pool server connection (SUBSCRIBE)
      if(!tx_mining_subscribe(client, mWorker)) {
        client.stop();
        MiningJobStop(job_pool, s_submition_map);
        continue;
      }

      strcpy(mWorker.wName, Settings.BtcWallet);
      strcpy(mWorker.wPass, Settings.PoolPassword);
      // STEP 2: Pool authorize work (Block Info)
      tx_mining_auth(client, mWorker.wName, mWorker.wPass); //Don't verifies authoritzation, TODO
      //tx_mining_auth2(client, mWorker.wName, mWorker.wPass); //Don't verifies authoritzation, TODO

      // STEP 3: Suggest pool difficulty
      tx_suggest_difficulty(client, currentPoolDifficulty);

      isMinerSuscribed=true;
      uint32_t time_now = millis();
      mLastTXtoPool = time_now;
      last_job_time = time_now;
    }

    //Check if pool is down for almost 5minutes and then restart connection with pool (1min=600000ms)
    if(checkPoolInactivity(KEEPALIVE_TIME_ms, POOLINACTIVITY_TIME_ms)){
      //Restart connection
      Serial.println("  Detected more than 2 min without data form stratum server. Closing socket and reopening...");
      client.stop();
      isMinerSuscribed=false;
      MiningJobStop(job_pool, s_submition_map);
      continue;
    }

    {
      uint32_t time_now = millis();
      if (time_now < last_job_time) //32bit wrap
        last_job_time = time_now;
      if (time_now >= last_job_time + 10*60*1000)  //10minutes without job
      {
        client.stop();
        isMinerSuscribed=false;
        MiningJobStop(job_pool, s_submition_map);
        continue;
      }
    }

    uint32_t hw_midstate[8];
    uint32_t diget_mid[8];
    uint32_t bake[16];
    #if defined(CONFIG_IDF_TARGET_ESP32)
    uint8_t sha_buffer_swap[128];
    #endif

    //Read pending messages from pool
    while(client.connected() && client.available())
    {
      String line = client.readStringUntil('\n');
      //Serial.println("  Received message from pool");
      stratum_method result = parse_mining_method(line);
      switch (result)
      {
          case MINING_NOTIFY:         if(parse_mining_notify(line, mJob))
                                      {
                                          {
                                            std::lock_guard<std::mutex> lock(s_job_mutex);
                                            s_job_request_list_sw.clear();
                                            #ifdef HARDWARE_SHA265
                                            s_job_request_list_hw.clear();
                                            #endif
                                          }
                                          //Increse templates readed
                                          templates++;
                                          job_pool++;
                                          s_working_current_job_id = job_pool & 0xFF; //Terminate current job in thread

                                          last_job_time = millis();
                                          mLastTXtoPool = last_job_time;

                                          uint32_t mh = hashes/1000000;
                                          Mhashes += mh;
                                          hashes -= mh*1000000;

                                          //Prepare data for new jobs
                                          mMiner=calculateMiningData(mWorker, mJob);

                                          memset(mMiner.bytearray_blockheader+80, 0, 128-80);
                                          mMiner.bytearray_blockheader[80] = 0x80;
                                          mMiner.bytearray_blockheader[126] = 0x02;
                                          mMiner.bytearray_blockheader[127] = 0x80;

                                          nerd_mids(diget_mid, mMiner.bytearray_blockheader);
                                          nerd_sha256_bake(diget_mid, mMiner.bytearray_blockheader+64, bake);

                                          #ifdef HARDWARE_SHA265
                                          #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
                                            esp_sha_acquire_hardware();
                                            sha_hal_hash_block(SHA2_256,  mMiner.bytearray_blockheader, 64/4, true);
                                            sha_hal_read_digest(SHA2_256, hw_midstate);
                                            esp_sha_release_hardware();
                                          #endif
                                          #endif

                                          #if defined(CONFIG_IDF_TARGET_ESP32)
                                          for (int i = 0; i < 32; ++i)
                                            ((uint32_t*)sha_buffer_swap)[i] = __builtin_bswap32(((const uint32_t*)(mMiner.bytearray_blockheader))[i]);
                                          #endif

                                          #ifdef RANDOM_NONCE
                                          nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                          #else
                                            #ifdef I2C_SLAVE
                                            if (!i2c_slave_vector.empty())
                                              nonce_pool = 0x10000000;
                                            else
                                            #endif
                                              nonce_pool = 0xDA54E700;  //nonce 0x00000000 is not possible, start from some random nonce
                                          #endif


                                          {
                                            std::lock_guard<std::mutex> lock(s_job_mutex);
                                            for (int i = 0; i < 4; ++ i)
                                            {
                                              #if 1
                                              JobPush( s_job_request_list_sw, job_pool, nonce_pool, NONCE_PER_JOB_SW, currentPoolDifficulty, mMiner.bytearray_blockheader, diget_mid, bake);
                                              #ifdef RANDOM_NONCE
                                              nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                              #else
                                              nonce_pool += NONCE_PER_JOB_SW;
                                              #endif
                                              #endif
                                              #ifdef HARDWARE_SHA265
                                                #if defined(CONFIG_IDF_TARGET_ESP32)
                                                  JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, sha_buffer_swap, hw_midstate, bake);
                                                #else
                                                  JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, mMiner.bytearray_blockheader, hw_midstate, bake);
                                                #endif
                                              #ifdef RANDOM_NONCE
                                              nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                              #else
                                              nonce_pool += NONCE_PER_JOB_HW;
                                              #endif
                                              #endif
                                            }
                                          }
                                          #ifdef I2C_SLAVE
                                          //Nonce for nonce_pool starts from 0x10000000
                                          //For i2c slave we give nonces from 0x20000000, that is 0x10000000 nonces per slave
                                          i2c_feed_slaves(i2c_slave_vector, job_pool & 0xFF, 0x20, currentPoolDifficulty, mMiner.bytearray_blockheader);
                                          #endif
                                      } else
                                      {
                                        Serial.println("Parsing error, need restart");
                                        client.stop();
                                        isMinerSuscribed=false;
                                        MiningJobStop(job_pool, s_submition_map);
                                      }
                                      break;
          case MINING_SET_DIFFICULTY: parse_mining_set_difficulty(line, currentPoolDifficulty);
                                      break;
          case STRATUM_SUCCESS:       {
                                        unsigned long id = parse_extract_id(line);
                                        auto itt = s_submition_map.find(id);
                                        if (itt != s_submition_map.end())
                                        {
                                          poolAccepted++;
                                          if (itt->second->diff > best_diff)
                                            best_diff = itt->second->diff;
                                          if (itt->second->is32bit)
                                            shares++;
                                          if (itt->second->isValid)
                                          {
                                            Serial.println("CONGRATULATIONS! Valid block found");
                                            valids++;
                                          }
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          case STRATUM_PARSE_ERROR:   {
                                        unsigned long id = parse_extract_id(line);
                                        auto itt = s_submition_map.find(id);
                                        if (itt != s_submition_map.end())
                                        {
                                          poolRejected++;
                                          Serial.printf("Refuse submition %d\n", id);
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          default:                    Serial.println("  Parsed JSON: unknown"); break;

      }
    }

    std::list<std::shared_ptr<JobResult>> job_result_list;
    #ifdef I2C_SLAVE
    if (i2c_slave_vector.empty() || job_pool == 0xFFFFFFFF)
    {
      vTaskDelay(50 / portTICK_PERIOD_MS); //Small delay
    } else
    {
      uint32_t time_start = millis();
      i2c_hit_slaves(i2c_slave_vector);
      vTaskDelay(5 / portTICK_PERIOD_MS);
      uint32_t nonces_done = 0;
      std::vector<uint32_t> nonce_vector = i2c_harvest_slaves(i2c_slave_vector, job_pool & 0xFF, nonces_done);
      hashes += nonces_done;
      for (size_t n = 0; n < nonce_vector.size(); ++n)
      {
        std::shared_ptr<JobResult> result = std::make_shared<JobResult>();
        ((uint32_t*)(mMiner.bytearray_blockheader+64+12))[0] = nonce_vector[n];
        if (nerd_sha256d_baked(diget_mid, mMiner.bytearray_blockheader+64, bake, result->hash))
        {
          result->id = job_pool;
          result->nonce = nonce_vector[n];
          result->nonce_count = 0;
          result->difficulty = diff_from_target(result->hash);
          job_result_list.push_back(result);
        }
      }
      uint32_t time_end = millis();
      //if (nonces_done > 16384)
        //Serial.printf("Harvest slaves in %dms hashes=%d\n", time_end - time_start, nonces_done);
      if (time_end > time_start)
      {
        uint32_t elapsed = time_end - time_start;
        if (elapsed < 50)
          vTaskDelay((50 - elapsed) / portTICK_PERIOD_MS);
      } else
        vTaskDelay(40 / portTICK_PERIOD_MS);
    }
    #else
    vTaskDelay(50 / portTICK_PERIOD_MS); //Small delay
    #endif


    if (job_pool != 0xFFFFFFFF)
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      job_result_list.insert(job_result_list.end(), s_job_result_list.begin(), s_job_result_list.end());
      s_job_result_list.clear();

#if 1
      while (s_job_request_list_sw.size() < 4)
      {
        JobPush( s_job_request_list_sw, job_pool, nonce_pool, NONCE_PER_JOB_SW, currentPoolDifficulty, mMiner.bytearray_blockheader, diget_mid, bake);
        #ifdef RANDOM_NONCE
        nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
        #else
        nonce_pool += NONCE_PER_JOB_SW;
        #endif
      }
#endif

      #ifdef HARDWARE_SHA265
      while (s_job_request_list_hw.size() < 4)
      {
        #if defined(CONFIG_IDF_TARGET_ESP32)
          JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, sha_buffer_swap, hw_midstate, bake);
        #else
          JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, mMiner.bytearray_blockheader, hw_midstate, bake);
        #endif
        #ifdef RANDOM_NONCE
        nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
        #else
        nonce_pool += NONCE_PER_JOB_HW;
        #endif
      }
      #endif
    }

    while (!job_result_list.empty())
    {
      std::shared_ptr<JobResult> res = job_result_list.front();
      job_result_list.pop_front();

      hashes += res->nonce_count;
      if (res->difficulty > currentPoolDifficulty && job_pool == res->id && res->nonce != 0xFFFFFFFF)
      {
        if (!client.connected())
          break;
        unsigned long sumbit_id = 0;
        tx_mining_submit(client, mWorker, mJob, res->nonce, sumbit_id);
        Serial.print("   - Current diff share: "); Serial.println(res->difficulty,12);
        Serial.print("   - Current pool diff : "); Serial.println(currentPoolDifficulty,12);
        Serial.print("   - TX SHARE: ");
        for (size_t i = 0; i < 32; i++)
            Serial.printf("%02x", res->hash[i]);
        Serial.println("");
        mLastTXtoPool = millis();

        std::shared_ptr<Submition> submition = std::make_shared<Submition>();
        submition->diff = res->difficulty;
        submition->is32bit = (res->hash[29] == 0 && res->hash[28] == 0);
        if (submition->is32bit)
        {
          submition->isValid = checkValid(res->hash, mMiner.bytearray_target);
        } else
          submition->isValid = false;

        s_submition_map.insert(std::make_pair(sumbit_id, submition));
        if (s_submition_map.size() > 32)
          s_submition_map.erase(s_submition_map.begin());
      }
    }
  }
}

//////////////////THREAD CALLS///////////////////

void minerWorkerSw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerSw Task!\n", miner_id);

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t hash[32];
  uint32_t wdt_counter = 0;
  while (1)
  {
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_sw.empty())
      {
        job = s_job_request_list_sw.front();
        s_job_request_list_sw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->difficulty = job->difficulty;
      result->nonce = 0xFFFFFFFF;
      result->id = job->id;
      result->nonce_count = job->nonce_count;
      uint8_t job_in_work = job->id & 0xFF;
      for (uint32_t n = 0; n < job->nonce_count; ++n)
      {
        ((uint32_t*)(job->sha_buffer+64+12))[0] = job->nonce_start+n;
        if (nerd_sha256d_baked(job->midstate, job->sha_buffer+64, job->bake, hash))
        {
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            result->difficulty = diff_hash;
            result->nonce = job->nonce_start+n;
            memcpy(result->hash, hash, 32);
          }
        }

        if ( (uint16_t)(n & 0xFF) == 0 &&s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n+1;
          break;
        }
      }
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#ifdef HARDWARE_SHA265

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    // C3/S2/S3 zero-write optimization
    // SHA_TEXT[9..14] are kept at zero for the whole hardware job.
    // Only registers that change between the first and second SHA blocks
    // are written for every nonce.
    REG_WRITE(&reg_addr_buf[0], data_words[0]);
    REG_WRITE(&reg_addr_buf[1], data_words[1]);
    REG_WRITE(&reg_addr_buf[2], data_words[2]);
    REG_WRITE(&reg_addr_buf[3], nonce);
    REG_WRITE(&reg_addr_buf[4], 0x00000080);
    REG_WRITE(&reg_addr_buf[5], 0x00000000);
    REG_WRITE(&reg_addr_buf[6], 0x00000000);
    REG_WRITE(&reg_addr_buf[7], 0x00000000);
    REG_WRITE(&reg_addr_buf[8], 0x00000000);
    // reg_addr_buf[9..14] deliberately not rewritten here
    REG_WRITE(&reg_addr_buf[15], 0x80020000);
}
static inline void nerd_sha_ll_fill_text_block_sha256_inter()
{
  uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);
  DPORT_INTERRUPT_DISABLE();
  REG_WRITE(&reg_addr_buf[0], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4));
  REG_WRITE(&reg_addr_buf[1], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4));
  REG_WRITE(&reg_addr_buf[2], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4));
  REG_WRITE(&reg_addr_buf[3], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4));
  REG_WRITE(&reg_addr_buf[4], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4));
  REG_WRITE(&reg_addr_buf[5], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4));
  REG_WRITE(&reg_addr_buf[6], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4));
  REG_WRITE(&reg_addr_buf[7], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4));
  DPORT_INTERRUPT_RESTORE();
  REG_WRITE(&reg_addr_buf[8], 0x00000080);
  // reg_addr_buf[9..14] deliberately stay zero
  REG_WRITE(&reg_addr_buf[15], 0x00010000);
}
static inline void nerd_sha_ll_read_digest(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4);
  ((uint32_t*)ptr)[7] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4);
  DPORT_INTERRUPT_RESTORE();
}


static inline bool nerd_sha_ll_read_digest_if(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  uint32_t last = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4);
  #if 1
  if ( (uint16_t)(last >> 16) != 0)
  {
    DPORT_INTERRUPT_RESTORE();
    return false;
  }
  #endif

  ((uint32_t*)ptr)[7] = last;
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4);
  DPORT_INTERRUPT_RESTORE();
  return true;
}

static inline void nerd_sha_ll_write_digest(void *digest_state)
{
    uint32_t *digest_state_words = (uint32_t *)digest_state;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_H_BASE);

    REG_WRITE(&reg_addr_buf[0], digest_state_words[0]);
    REG_WRITE(&reg_addr_buf[1], digest_state_words[1]);
    REG_WRITE(&reg_addr_buf[2], digest_state_words[2]);
    REG_WRITE(&reg_addr_buf[3], digest_state_words[3]);
    REG_WRITE(&reg_addr_buf[4], digest_state_words[4]);
    REG_WRITE(&reg_addr_buf[5], digest_state_words[5]);
    REG_WRITE(&reg_addr_buf[6], digest_state_words[6]);
    REG_WRITE(&reg_addr_buf[7], digest_state_words[7]);
}

static inline void nerd_sha_hal_wait_idle()
{
    while (REG_READ(SHA_BUSY_REG))
    {}
}

//#define VALIDATION
void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHw Task!\n", miner_id);

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t interResult[64];
  uint8_t hash[32];
  uint8_t digest_mid[32];
  uint8_t sha_buffer[64];
  uint32_t wdt_counter = 0;

#ifdef VALIDATION
  uint8_t doubleHash[32];
  uint32_t diget_mid[8];
  uint32_t bake[16];
#endif

  while (1)
  {
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_hw.empty())
      {
        job = s_job_request_list_hw.front();
        s_job_request_list_hw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->id = job->id;
      result->nonce = 0xFFFFFFFF;
      result->nonce_count = job->nonce_count;
      result->difficulty = job->difficulty;
      uint8_t job_in_work = job->id & 0xFF;
      memcpy(digest_mid, job->midstate, sizeof(digest_mid));
      memcpy(sha_buffer, job->sha_buffer+64, sizeof(sha_buffer));
#ifdef VALIDATION
      nerd_mids(diget_mid, job->sha_buffer);
      nerd_sha256_bake(diget_mid, job->sha_buffer+64, bake);
#endif

      esp_sha_acquire_hardware();
      REG_WRITE(SHA_MODE_REG, SHA2_256);

      // Initialize constant-zero message words once per hardware job.
      // The per-nonce helpers intentionally leave SHA_TEXT[9..14] untouched.
      uint32_t *sha_text_regs = (uint32_t *)(SHA_TEXT_BASE);
      REG_WRITE(&sha_text_regs[9],  0x00000000);
      REG_WRITE(&sha_text_regs[10], 0x00000000);
      REG_WRITE(&sha_text_regs[11], 0x00000000);
      REG_WRITE(&sha_text_regs[12], 0x00000000);
      REG_WRITE(&sha_text_regs[13], 0x00000000);
      REG_WRITE(&sha_text_regs[14], 0x00000000);

      uint32_t nend = job->nonce_start + job->nonce_count;
      for (uint32_t n = job->nonce_start; n < nend; ++n)
      {
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_write_digest(digest_mid);
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256(sha_buffer, n);
        //sha_ll_continue_block(SHA2_256);
        REG_WRITE(SHA_CONTINUE_REG, 1);

        sha_ll_load(SHA2_256);
        nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256_inter();
        //sha_ll_start_block(SHA2_256);
        REG_WRITE(SHA_START_REG, 1);
        sha_ll_load(SHA2_256);
        nerd_sha_hal_wait_idle();
        if (nerd_sha_ll_read_digest_if(hash))
        {
          //Serial.printf("Hw 16bit Share, nonce=0x%X\n", n);
#ifdef VALIDATION
          //Validation
          ((uint32_t*)(job->sha_buffer+64+12))[0] = n;
          nerd_sha256d_baked(diget_mid, job->sha_buffer+64, bake, doubleHash);
          for (int i = 0; i < 32; ++i)
          {
            if (hash[i] != doubleHash[i])
            {
              Serial.println("***HW sha256 esp32s3 bug detected***");
              break;
            }
          }
#endif
          //~5 per second
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            if (isSha256Valid(hash))
            {
              result->difficulty = diff_hash;
              result->nonce = n;
              memcpy(result->hash, hash, sizeof(hash));
            }
          }
        }
        if (
             (uint8_t)(n & 0xFF) == 0 &&
             s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n-job->nonce_start+1;
          break;
        }
      }
      esp_sha_release_hardware();
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#endif  //#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

#if defined(CONFIG_IDF_TARGET_ESP32)

static inline bool nerd_sha_ll_read_digest_swap_if(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  uint32_t fin = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
  if ( (uint32_t)(fin & 0xFFFF) != 0)
  {
    DPORT_INTERRUPT_RESTORE();
    return false;
  }
  ((uint32_t*)ptr)[7] = __builtin_bswap32(fin);
  ((uint32_t*)ptr)[0] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 0 * 4));
  ((uint32_t*)ptr)[1] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 1 * 4));
  ((uint32_t*)ptr)[2] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 2 * 4));
  ((uint32_t*)ptr)[3] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 3 * 4));
  ((uint32_t*)ptr)[4] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 4 * 4));
  ((uint32_t*)ptr)[5] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 5 * 4));
  ((uint32_t*)ptr)[6] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 6 * 4));
  DPORT_INTERRUPT_RESTORE();
  return true;
}

static inline void nerd_sha_ll_read_digest(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 6 * 4);
  ((uint32_t*)ptr)[7] = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
  DPORT_INTERRUPT_RESTORE();
}

static inline void nerd_sha_hal_wait_idle()
{
    while (DPORT_REG_READ(SHA_256_BUSY_REG))
    {}
}

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = data_words[3];
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
    reg_addr_buf[8]  = data_words[8];
    reg_addr_buf[9]  = data_words[9];
    reg_addr_buf[10] = data_words[10];
    reg_addr_buf[11] = data_words[11];
    reg_addr_buf[12] = data_words[12];
    reg_addr_buf[13] = data_words[13];
    reg_addr_buf[14] = data_words[14];
    reg_addr_buf[15] = data_words[15];
}

static inline void nerd_sha_ll_fill_text_block_sha256_upper(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = __builtin_bswap32(nonce);
#if 1
    reg_addr_buf[4]  = 0x80000000;
    reg_addr_buf[5]  = 0x00000000;
    reg_addr_buf[6]  = 0x00000000;
    reg_addr_buf[7]  = 0x00000000;
    reg_addr_buf[8]  = 0x00000000;
    reg_addr_buf[9]  = 0x00000000;
    reg_addr_buf[10] = 0x00000000;
    reg_addr_buf[11] = 0x00000000;
    reg_addr_buf[12] = 0x00000000;
    reg_addr_buf[13] = 0x00000000;
    reg_addr_buf[14] = 0x00000000;
    reg_addr_buf[15] = 0x00000280;
#else
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
    reg_addr_buf[8]  = data_words[8];
    reg_addr_buf[9]  = data_words[9];
    reg_addr_buf[10] = data_words[10];
    reg_addr_buf[11] = data_words[11];
    reg_addr_buf[12] = data_words[12];
    reg_addr_buf[13] = data_words[13];
    reg_addr_buf[14] = data_words[14];
    reg_addr_buf[15] = data_words[15];
#endif
}

static inline void nerd_sha_ll_fill_text_block_sha256_double()
{
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

#if 0
    //No change
    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = data_words[3];
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
#endif
    reg_addr_buf[8]  = 0x80000000;
    reg_addr_buf[9]  = 0x00000000;
    reg_addr_buf[10] = 0x00000000;
    reg_addr_buf[11] = 0x00000000;
    reg_addr_buf[12] = 0x00000000;
    reg_addr_buf[13] = 0x00000000;
    reg_addr_buf[14] = 0x00000000;
    reg_addr_buf[15] = 0x00000100;
}

void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHwEsp32D Task!\n", miner_id);

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t hash[32];
  uint8_t sha_buffer[128];

  while (1)
  {
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_hw.empty())
      {
        job = s_job_request_list_hw.front();
        s_job_request_list_hw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->id = job->id;
      result->nonce = 0xFFFFFFFF;
      result->nonce_count = job->nonce_count;
      result->difficulty = job->difficulty;
      uint8_t job_in_work = job->id & 0xFF;
      memcpy(sha_buffer, job->sha_buffer, 80);

      esp_sha_lock_engine(SHA2_256);
      for (uint32_t n = 0; n < job->nonce_count; ++n)
      {
        //((uint32_t*)(sha_buffer+64+12))[0] = __builtin_bswap32(job->nonce_start+n);

        //sha_hal_hash_block(SHA2_256, s_test_buffer, 64/4, true);
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256(sha_buffer);
        sha_ll_start_block(SHA2_256);

        //sha_hal_hash_block(SHA2_256, s_test_buffer+64, 64/4, false);
        nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256_upper(sha_buffer+64, job->nonce_start+n);
        sha_ll_continue_block(SHA2_256);

        nerd_sha_hal_wait_idle();
        sha_ll_load(SHA2_256);

        //sha_hal_hash_block(SHA2_256, interResult, 64/4, true);
        nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256_double();
        sha_ll_start_block(SHA2_256);

        nerd_sha_hal_wait_idle();
        sha_ll_load(SHA2_256);
        if (nerd_sha_ll_read_digest_swap_if(hash))
        {
          //~5 per second
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            if (isSha256Valid(hash))
            {
              result->difficulty = diff_hash;
              result->nonce = job->nonce_start+n;
              memcpy(result->hash, hash, sizeof(hash));
            }
          }
        }
        if (
             (uint8_t)(n & 0xFF) == 0 &&
             s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n+1;
          break;
        }
      }
      esp_sha_unlock_engine(SHA2_256);
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    esp_task_wdt_reset();
  }
}

#endif  //CONFIG_IDF_TARGET_ESP32

#endif  //HARDWARE_SHA265


#define DELAY 100
#define REDRAW_EVERY 10

void restoreStat() {
  if(!Settings.saveStats) return;
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.printf("[MONITOR] NVS partition is full or has invalid version, erasing...\n");
    nvs_flash_init();
  }

  ret = nvs_open("state", NVS_READWRITE, &stat_handle);

  size_t required_size = sizeof(double);
  nvs_get_blob(stat_handle, "best_diff", &best_diff, &required_size);
  nvs_get_u32(stat_handle, "Mhashes", &Mhashes);
  uint32_t nv_shares, nv_valids;
  nvs_get_u32(stat_handle, "shares", &nv_shares);
  nvs_get_u32(stat_handle, "valids", &nv_valids);
  shares = nv_shares;
  valids = nv_valids;
  nvs_get_u32(stat_handle, "templates", &templates);
  nvs_get_u64(stat_handle, "upTime", &upTime);

  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &best_diff, sizeof(best_diff));
  crc = crc32_add(crc, &Mhashes, sizeof(Mhashes));
  crc = crc32_add(crc, &nv_shares, sizeof(nv_shares));
  crc = crc32_add(crc, &nv_valids, sizeof(nv_valids));
  crc = crc32_add(crc, &templates, sizeof(templates));
  crc = crc32_add(crc, &upTime, sizeof(upTime));
  crc = crc32_finish(crc);

  uint32_t nv_crc;
  nvs_get_u32(stat_handle, "crc32", &nv_crc);
  if (nv_crc != crc)
  {
    best_diff = 0.0;
    Mhashes = 0;
    shares = 0;
    valids = 0;
    templates = 0;
    upTime = 0;
  }
}

void saveStat() {
  if(!Settings.saveStats) return;
  Serial.printf("[MONITOR] Saving stats\n");
  nvs_set_blob(stat_handle, "best_diff", &best_diff, sizeof(best_diff));
  nvs_set_u32(stat_handle, "Mhashes", Mhashes);
  nvs_set_u32(stat_handle, "shares", shares);
  nvs_set_u32(stat_handle, "valids", valids);
  nvs_set_u32(stat_handle, "templates", templates);
  nvs_set_u64(stat_handle, "upTime", upTime);

  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &best_diff, sizeof(best_diff));
  crc = crc32_add(crc, &Mhashes, sizeof(Mhashes));
  uint32_t nv_shares = shares;
  uint32_t nv_valids = valids;
  crc = crc32_add(crc, &nv_shares, sizeof(nv_shares));
  crc = crc32_add(crc, &nv_valids, sizeof(nv_valids));
  crc = crc32_add(crc, &templates, sizeof(templates));
  crc = crc32_add(crc, &upTime, sizeof(upTime));
  crc = crc32_finish(crc);
  nvs_set_u32(stat_handle, "crc32", crc);
}

void resetStat() {
    Serial.printf("[MONITOR] Resetting NVS stats\n");
    templates = hashes = Mhashes = totalKHashes = elapsedKHs = upTime = shares = valids = 0;
    best_diff = 0.0;
    saveStat();
}


static const char STATS_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Miner</title>
<style>
*{box-sizing:border-box}
body{
  font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
  background:#101114;color:#eee;margin:0;padding:22px 14px;
  text-align:center
}
.wrap{max-width:900px;margin:0 auto}
.topline{font-size:.9rem;font-weight:700;letter-spacing:.14em;color:#9fb7d8;margin-bottom:4px}
h1{font-size:1.55rem;margin:0;font-weight:800}
.subtitle{color:#9aa0aa;margin:2px 0 22px;font-size:.95rem}
.grid{display:grid;gap:10px}
.speed-grid{grid-template-columns:repeat(3,1fr)}
.status-grid{grid-template-columns:repeat(2,1fr);margin-top:10px}
.stats-grid{grid-template-columns:repeat(4,1fr);margin-top:10px}
.card{
  background:#1a1c21;border-radius:13px;padding:14px 10px;
  min-height:86px;display:flex;flex-direction:column;
  align-items:center;justify-content:center
}
.label{color:#9fb7d8;font-size:.82rem;margin-bottom:4px}
.value{font-size:1.65rem;font-weight:800;line-height:1.12}
.pool{
  margin:20px 0 14px;padding:0 8px;
  color:#c9cdd4;font-size:.9rem
}
.pool strong{
  display:block;color:#fff;font-size:1.05rem;
  margin-top:3px;overflow-wrap:anywhere
}
.footer{
  margin-top:18px;color:#8f959e;font-size:.82rem;
  line-height:1.6
}
.config-link{
  color:inherit;
  font-size:inherit;
  font-weight:bold;
  text-decoration:underline;
}
@media(max-width:650px){
  .speed-grid{grid-template-columns:1fr}
  .stats-grid{grid-template-columns:repeat(2,1fr)}
}
.watermark{
  margin-top:24px;
  padding:10px 0;
  text-align:center;
  font-size:11px;
  font-weight:bold;
  letter-spacing:.5px;
  color:#666;
  opacity:.75;
}
</style>
</head>
<body>
<div class="wrap">
  <div class="topline" id="chipModel">ESP32</div>
  <h1>NerdMiner v2</h1>
  <div class="subtitle">Local stats</div>

  <div class="grid speed-grid">
    <div class="card"><div class="label">Current average</div><div class="value" id="cur">--</div></div>
    <div class="card"><div class="label">10s average</div><div class="value" id="a10">--</div></div>
    <div class="card"><div class="label">60s average</div><div class="value" id="a60">--</div></div>
  </div>

  <div class="grid status-grid">
    <div class="card"><div class="label">Wi-Fi signal</div><div class="value" id="rssi">--</div></div>
    <div class="card"><div class="label">Core temperature</div><div class="value" id="temp">--</div></div>
  </div>

  <div class="pool">
    Pool
    <strong id="pool">--</strong>
  </div>

  <div class="grid stats-grid">
    <div class="card"><div class="label">Accepted</div><div class="value" id="ok">--</div></div>
    <div class="card"><div class="label">Rejected</div><div class="value" id="rej">--</div></div>
    <div class="card"><div class="label">Templates</div><div class="value" id="tpl">--</div></div>
    <div class="card"><div class="label">Best diff</div><div class="value" id="bd">--</div></div>
  </div>

  <div class="footer">
  <div id="uptime">Uptime: --</div>
  <div id="ip">IP: --</div>
  <div><a class="config-link" href="/config">Web Manager</a></div>
</div>

<script>
const $=id=>document.getElementById(id);

function fmtUptime(sec){
  sec=Math.max(0,Math.floor(sec||0));
  const d=Math.floor(sec/86400); sec%=86400;
  const h=Math.floor(sec/3600); sec%=3600;
  const m=Math.floor(sec/60);
  const s=sec%60;
  const hh=String(h).padStart(2,'0');
  const mm=String(m).padStart(2,'0');
  const ss=String(s).padStart(2,'0');
  return (d?d+' nap ':'')+hh+':'+mm+':'+ss;
}

async function tick(){
  try{
    const r=await fetch('/api/stats',{cache:'no-store'});
    const s=await r.json();

    $('cur').textContent=s.hashrate_khs.toFixed(1)+' kH/s';
    $('a10').textContent=s.avg10_khs.toFixed(1)+' kH/s';
    $('a60').textContent=s.avg60_khs.toFixed(1)+' kH/s';
    $('rssi').textContent=s.rssi_dbm+' dBm';
    $('temp').textContent=s.temp_c.toFixed(1)+' °C';
    $('pool').textContent=(s.pool||'--')+(s.pool_port?':'+s.pool_port:'');
    $('chipModel').textContent = s.chip_model || 'ESP32';
    $('ok').textContent=s.accepted;
    $('rej').textContent=s.rejected;
    $('tpl').textContent=s.templates;
    $('bd').textContent=s.best_diff.toFixed(6);
    $('uptime').textContent='Uptime: '+fmtUptime(s.uptime_s);
    $('ip').textContent='IP: '+s.ip;
  }catch(e){}
  setTimeout(tick,2000);
}
tick();
</script>
<div class="watermark">
  Misu's Mining Lab • Hashing on NerdMiner • ESP32 Web Manager
</div>
</body>
</html>
)HTML";

static const char CONFIG_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="hu">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Miner - Web Manager</title>

<style>
body{
  margin:0;
  padding:20px;
  background:#101114;
  color:#f2f2f2;
  font-family:Arial,sans-serif;
  text-align:center;
}

.wrap{
  max-width:760px;
  margin:0 auto;
}

h1{
  margin-bottom:5px;
}

.subtitle{
  color:#aaa;
  margin-bottom:25px;
}

.card{
  background:#1b1d21;
  border-radius:12px;
  padding:16px;
  margin:10px 0;
  text-align:left;
}

.card h2{
  margin-top:0;
  text-align:center;
}

.row{
  display:flex;
  justify-content:space-between;
  gap:15px;
  padding:7px 0;
  border-bottom:1px solid #2a2d32;
}

.row:last-child{
  border-bottom:0;
}

.value{
  text-align:right;
  overflow-wrap:anywhere;
}

a{
  color:#ddd;
}

.wifi-actions{
  margin-top:18px;
  text-align:center;
}

button{
  background:#2b2e34;
  color:#f2f2f2;
  border:1px solid #444;
  border-radius:8px;
  padding:11px 18px;
  font-size:15px;
  cursor:pointer;
}

button:hover{
  background:#363a41;
}

#scanStatus{
  margin:12px 0;
  color:#aaa;
  text-align:center;
}

.network{
  background:#24272c;
  border-radius:8px;
  padding:10px 12px;
  margin-top:8px;
  text-align:left;
}

.network-current{
  border:1px solid #777;
}

.network-title{
  font-weight:bold;
  margin-bottom:5px;
}

.network-info{
  font-size:13px;
  color:#bbb;
  line-height:1.5;
}

.network{
  cursor:pointer;
}

.network:hover{
  background:#30343a;
}

.network-selected{
  border:1px solid #aaa;
  background:#30343a;
}

.selected-network{
  margin-top:15px;
  padding:12px;
  background:#24272c;
  border-radius:8px;
  text-align:center;
  line-height:1.6;
}

.config-options{
  margin-top:15px;
  padding-top:12px;
  border-top:1px solid #3a3d42;
}

.check-row{
  display:block;
  margin:10px 0;
  cursor:pointer;
}

.check-row input{
  margin-right:8px;
}

#saveWifiButton{
  margin-top:10px;
}

#saveWifiStatus{
  margin-top:10px;
  color:#aaa;
}

.form-row{
  display:grid;
  grid-template-columns:160px 1fr;
  align-items:center;
  gap:12px;
  margin:12px 0;
  text-align:left;
}

.form-row input{
  width:100%;
  box-sizing:border-box;
  background:#24272c;
  color:#f2f2f2;
  border:1px solid #444;
  border-radius:7px;
  padding:10px;
  font-size:14px;
}

.pool-actions{
  margin-top:18px;
  text-align:center;
}

#savePoolStatus{
  margin-top:10px;
  color:#aaa;
}

@media(max-width:600px){
  .form-row{
    grid-template-columns:1fr;
    gap:5px;
  }
}

.system-actions{
  margin-top:18px;
  padding-top:15px;
  border-top:1px solid #3a3d42;
  text-align:center;
}

.danger-zone{
  margin-top:18px;
  padding-top:15px;
  border-top:1px solid #3a3d42;
}

.warning-text{
  margin-top:9px;
  font-size:13px;
  color:#aaa;
  line-height:1.5;
}

#systemStatus{
  margin-top:10px;
  color:#aaa;
}

.back{
  margin-top:20px;
}
.watermark{
  margin-top:24px;
  padding:10px 0;
  text-align:center;
  font-size:11px;
  font-weight:bold;
  letter-spacing:.5px;
  color:#666;
  opacity:.75;
}
</style>
</head>

<body>

<div class="wrap">

  <h1>NerdMiner v2</h1>
  <div class="subtitle">Web Manager</div>

  <div class="card">
    <h2>Wi-Fi</h2>

    <div class="row">
      <span>SSID</span>
      <span class="value" id="ssid">--</span>
    </div>

    <div class="row">
      <span>Current BSSID</span>
      <span class="value" id="currentBssid">--</span>
    </div>

    <div class="row">
      <span>Channel</span>
      <span class="value" id="currentChannel">--</span>
    </div>

    <div class="row">
      <span>ESP MAC</span>
      <span class="value" id="mac">--</span>
    </div>

    <div class="row">
      <span>BSSID lock</span>
      <span class="value" id="bssidLock">--</span>
    </div>

    <div class="row">
      <span>Saved BSSID</span>
      <span class="value" id="savedBssid">--</span>
    </div>

    <div class="row">
      <span>Saved channel</span>
      <span class="value" id="savedChannel">--</span>
    </div>

    <div class="row">
      <span>Fallback</span>
      <span class="value" id="fallback">--</span>
    </div>

    <div class="wifi-actions">
       <button type="button" id="scanButton">Scan networks</button>
    </div>

    <div id="scanStatus"></div>
    <div id="wifiNetworks"></div>
<div id="selectedNetwork" class="selected-network" style="display:none">
    <div><strong>Selected AP</strong></div>
    <div id="selectedSsid">--</div>
    <div id="selectedBssid">--</div>
    <div id="selectedChannel">--</div>
    <div class="config-options">

  <label class="check-row">
    <input type="checkbox" id="enableBssidLock">
    Enable BSSID lock
  </label>

  <label class="check-row">
    <input type="checkbox" id="enableFallback" checked>
    Fallback to another AP with the same SSID
  </label>

  <button type="button" id="saveWifiButton">
    Save Wi-Fi settings
  </button>

  <div id="saveWifiStatus"></div>

  </div>
</div>
  </div>

  <div class="card">
  <h2>Pool</h2>

  <div class="form-row">
    <label for="poolHost">Host</label>
    <input type="text" id="poolHost" maxlength="80">
  </div>

  <div class="form-row">
    <label for="poolPortInput">Port</label>
    <input type="number" id="poolPortInput" min="1" max="65535">
  </div>

  <div class="form-row">
    <label for="poolWallet">Wallet.worker</label>
    <input type="text" id="poolWallet" maxlength="79">
  </div>

  <div class="form-row">
    <label for="poolPassword">Pool password</label>
    <input type="password"
           id="poolPassword"
           maxlength="79"
           placeholder="Leave blank to keep unchanged">
  </div>

  <div class="pool-actions">
    <button type="button" id="savePoolButton">
      Save pool settings
    </button>

    <div id="savePoolStatus"></div>
  </div>
</div>

<div class="card">
  <h2>System</h2>

  <div class="row">
    <span>IP address</span>
    <span class="value" id="systemIp">--</span>
  </div>

  <div class="row">
    <span>Chip</span>
    <span class="value" id="chipModel">--</span>
  </div>

  <div class="row">
    <span>Chip revision</span>
    <span class="value" id="chipRevision">--</span>
  </div>

  <div class="row">
    <span>CPU</span>
    <span class="value" id="cpuMHz">--</span>
  </div>

  <div class="row">
    <span>Free heap</span>
    <span class="value" id="freeHeap">--</span>
  </div>

  <div class="row">
    <span>ESP-IDF / SDK</span>
    <span class="value" id="sdkVersion">--</span>
  </div>

  <div class="row">
    <span>Uptime</span>
    <span class="value" id="systemUptime">--</span>
  </div>

 <div class="system-actions">

  <button type="button" id="rebootButton">
    Restart device
  </button>

  <div class="danger-zone">
    <button type="button" id="wifiResetButton">
      Reset Wi-Fi settings
    </button>

    <div class="warning-text">
      Configuration AP mode starts after restart.
      Pool and wallet settings will be preserved.
    </div>
  </div>

  <div id="systemStatus"></div>

</div>
</div>

  <div class="back">
    <a href="/">Back to status</a>
  </div>

</div>

<script>

const $ = id => document.getElementById(id);

function fmtUptime(sec)
{
  sec = Math.max(0, Math.floor(sec || 0));

  const d = Math.floor(sec / 86400);
  sec %= 86400;

  const h = Math.floor(sec / 3600);
  sec %= 3600;

  const m = Math.floor(sec / 60);
  const s = sec % 60;

  const hh = String(h).padStart(2, '0');
  const mm = String(m).padStart(2, '0');
  const ss = String(s).padStart(2, '0');

  return (d ? d + ' nap ' : '') + hh + ':' + mm + ':' + ss;
}

let selectedWifiSSID = '';
let selectedWifiBSSID = '';
let selectedWifiChannel = 0;

// WM_SUPPORT_INTEGRATION_BEGIN

let wmSupportInitialPool = null;
let wmSupportPendingUrl = '';

function wmNormalizePoolHost(value)
{
  return String(value || '')
    .trim()
    .toLowerCase()
    .replace(/^stratum\+tcp:\/\//, '')
    .replace(/^https?:\/\//, '')
    .split('/')[0]
    .split(':')[0]
    .replace(/\.$/, '');
}

function wmGetSupportUrl(value)
{
  const host = wmNormalizePoolHost(value);

  const matches = (base) =>
    host === base || host.endsWith('.' + base);

  if (matches('xec.hmpool.io'))
    return 'https://xec.hmpool.io?ref=HM-39uakb';

  if (matches('digi.hmpool.io'))
    return 'https://digi.hmpool.io?ref=HM-qg3swe';

  if (matches('btc.hmpool.io'))
    return 'https://hmpool.io?ref=HM-dk6cuv';

  if (matches('bch.hmpool.io'))
    return 'https://bch.hmpool.io?ref=HM-r6zdkz';

  return '';
}

function wmFindRestartButton()
{
  const byId = document.getElementById('rebootButton');

  if (byId)
    return byId;

  return Array.from(document.querySelectorAll('button')).find((button) =>
    /restart device/i.test(button.textContent || '')
  );
}

function wmRenderSupportState()
{
  const note = document.getElementById('wmSupportRestartNote');

  if (!note)
    return;

  note.style.display = wmSupportPendingUrl ? 'block' : 'none';
}

async function wmRefreshSupportState(setInitial)
{
  try
  {
    const response = await fetch('/api/config', {cache:'no-store'});
    const config = await response.json();

    const currentPool = wmNormalizePoolHost(config.pool || '');

    if (setInitial || wmSupportInitialPool === null)
      wmSupportInitialPool = currentPool;

    if (currentPool !== wmSupportInitialPool)
      wmSupportPendingUrl = wmGetSupportUrl(currentPool);
    else
      wmSupportPendingUrl = '';

    wmRenderSupportState();
  }
  catch (e)
  {
    console.log('Support-link state check failed:', e);
  }
}

function wmInstallSupportHooks()
{
  const saveButton = document.getElementById('savePoolButton');
  const originalRestartButton = wmFindRestartButton();

  if (!originalRestartButton)
    return;

  const restartButton = originalRestartButton.cloneNode(true);

  restartButton.removeAttribute('onclick');
  originalRestartButton.replaceWith(restartButton);

  const note = document.createElement('div');

  note.id = 'wmSupportRestartNote';
  note.innerHTML =
    'Optional support:<br>' +
    'After restart you will be redirected to the pool affiliate page.<br>' +
    'If you check your miner there, you support Web Manager development.';

  note.style.display = 'none';
  note.style.marginTop = '8px';
  note.style.fontSize = '12px';
  note.style.opacity = '0.60';
  note.style.textAlign = 'center';

  restartButton.insertAdjacentElement('afterend', note);

  if (saveButton)
  {
    saveButton.addEventListener('click', function()
    {
      setTimeout(function()
      {
        wmRefreshSupportState(false);
      }, 600);

      setTimeout(function()
      {
        wmRefreshSupportState(false);
      }, 1400);
    });
  }

  restartButton.addEventListener('click', function()
  {
    const supportUrl = wmSupportPendingUrl;

    let message = 'Restart device?';

    if (supportUrl)
    {
      message +=
        '\n\nAfter restart you will be redirected to the pool affiliate page.' +
        '\nIf you check your miner there, you support Web Manager development through the pool affiliate program.' +
        '\n\nThis is optional.';
    }

    if (!confirm(message))
      return;

    restartButton.disabled = true;
    restartButton.textContent = 'Restarting...';

    fetch('/api/reboot', {
      method: 'POST',
      cache: 'no-store'
    }).catch(function()
    {
      // Connection loss is expected during restart.
    });

    if (supportUrl)
    {
      setTimeout(function()
      {
        window.location.href = supportUrl;
      }, 700);
    }
  });

  wmRefreshSupportState(true);
}

if (document.readyState === 'loading')
{
  document.addEventListener(
    'DOMContentLoaded',
    wmInstallSupportHooks
  );
}
else
{
  wmInstallSupportHooks();
}

// WM_SUPPORT_INTEGRATION_END
async function loadConfig()
{
  try
  {
    const r = await fetch('/api/config', {cache:'no-store'});
    const c = await r.json();

    $('ssid').textContent = c.ssid || '--';
    $('currentBssid').textContent = c.current_bssid || '--';
    $('currentChannel').textContent = c.current_channel || '--';
    $('mac').textContent = c.mac || '--';

    $('bssidLock').textContent = c.bssid_lock ? 'ON' : 'OFF';
    $('savedBssid').textContent = c.saved_bssid || '--';
    $('savedChannel').textContent = c.saved_channel || '--';
    $('fallback').textContent = c.fallback ? 'ON' : 'OFF';
    $('enableBssidLock').checked = !!c.bssid_lock;
    $('enableFallback').checked = !!c.fallback;

    $('poolHost').value = c.pool || '';
    $('poolPortInput').value = c.pool_port || '';
    $('poolWallet').value = c.wallet || '';
    $('poolPassword').value = '';
    $('systemIp').textContent = c.system_ip || '--';
    $('chipModel').textContent = c.chip_model || '--';
    $('chipRevision').textContent = c.chip_revision ?? '--';
    $('cpuMHz').textContent = c.cpu_mhz ? c.cpu_mhz + ' MHz' : '--';
    $('freeHeap').textContent = c.free_heap ? c.free_heap + ' byte' : '--';
    $('sdkVersion').textContent = c.sdk_version || '--';
    $('systemUptime').textContent = fmtUptime(c.uptime_s || 0);
  }
  catch(e)
  {
  }
}

async function scanWiFi()
{
  const button = $('scanButton');
  const status = $('scanStatus');
  const list = $('wifiNetworks');

  button.disabled = true;
  status.textContent = 'Scan networks...';

  try
  {
    const r = await fetch('/api/wifi-scan', {cache:'no-store'});

    if (r.status === 202)
    {
      setTimeout(scanWiFi, 1000);
      return;
    }

    if (!r.ok)
      throw new Error('scan failed');

    const data = await r.json();

    list.innerHTML = '';

    if (!data.networks || data.networks.length === 0)
    {
      status.textContent = 'No Wi-Fi networks found.';
      button.disabled = false;
      return;
    }

    data.networks.sort((a, b) => b.rssi - a.rssi);

    for (const ap of data.networks)
    {
      const item = document.createElement('div');
      item.className = 'network' + (ap.current ? ' network-current' : '');

      const title = document.createElement('div');
      title.className = 'network-title';
      title.textContent =
        (ap.ssid || '(rejtett SSID)') +
        (ap.current ? '  •  current connection' : '');

      const info = document.createElement('div');
      info.className = 'network-info';

      info.textContent =
        'RSSI: ' + ap.rssi + ' dBm' +
        '   |   CH: ' + ap.channel +
        '   |   BSSID: ' + ap.bssid;

      item.addEventListener('click', () =>
      {
        selectedWifiSSID = ap.ssid || '';
        selectedWifiBSSID = ap.bssid || '';
        selectedWifiChannel = ap.channel || 0;

        document.querySelectorAll('.network').forEach(el =>
          el.classList.remove('network-selected')
        );

        item.classList.add('network-selected');

        $('selectedNetwork').style.display = 'block';
        $('selectedSsid').textContent =
          'SSID: ' + (ap.ssid || '(rejtett SSID)');
        $('selectedBssid').textContent =
          'BSSID: ' + ap.bssid;
        $('selectedChannel').textContent =
          'Channel: ' + ap.channel;
      });
      item.appendChild(title);
      item.appendChild(info);
      list.appendChild(item);
    }

    status.textContent =
      data.networks.length + ' network(s) found.';
  }
  catch(e)
  {
    status.textContent = 'Wi-Fi scan failed.';
  }

  button.disabled = false;
}

$('saveWifiButton').addEventListener('click', async () =>
{
  const status = $('saveWifiStatus');

  if (!selectedWifiBSSID || !selectedWifiChannel)
  {
    status.textContent = 'Select a Wi-Fi AP first.';
    return;
  }

  const currentSSID = $('ssid').textContent;

  if (selectedWifiSSID !== currentSSID)
  {
    status.textContent =
      'A different SSID cannot currently be selected. Wi-Fi password configuration is required.';
    return;
  }

  const params = new URLSearchParams();

  params.append('bssid', selectedWifiBSSID);
  params.append('channel', selectedWifiChannel);
  params.append(
    'lock',
    $('enableBssidLock').checked ? '1' : '0'
  );
  params.append(
    'fallback',
    $('enableFallback').checked ? '1' : '0'
  );

  status.textContent = 'Saving...';

  try
  {
    const r = await fetch('/api/wifi-config',
    {
      method: 'POST',
      headers:
      {
        'Content-Type': 'application/x-www-form-urlencoded'
      },
      body: params.toString()
    });

    if (!r.ok)
      throw new Error('save failed');

    const result = await r.json();

    if (result.ok)
      status.textContent = 'Settings saved.';
    else
      status.textContent = result.error || 'Save failed.';
  }
  catch(e)
  {
    status.textContent = 'Save failed.';
  }
});

$('scanButton').addEventListener('click', scanWiFi);

loadConfig();

$('savePoolButton').addEventListener('click', async () =>
{
  const status = $('savePoolStatus');

  const host = $('poolHost').value.trim();
  const port = parseInt($('poolPortInput').value, 10);
  const wallet = $('poolWallet').value.trim();
  const password = $('poolPassword').value;

  if (!host)
  {
    status.textContent = 'Pool host cannot be empty.';
    return;
  }

  if (!port || port < 1 || port > 65535)
  {
    status.textContent = 'Invalid port.';
    return;
  }

  if (!wallet)
  {
    status.textContent = 'Wallet / worker cannot be empty.';
    return;
  }

  const params = new URLSearchParams();

  params.append('host', host);
  params.append('port', port.toString());
  params.append('wallet', wallet);

  // Empty password = keep the current password unchanged.
  if (password.length > 0)
    params.append('password', password);

  status.textContent = 'Saving...';

  try
  {
    const r = await fetch('/api/pool-config',
    {
      method: 'POST',
      headers:
      {
        'Content-Type': 'application/x-www-form-urlencoded'
      },
      body: params.toString()
    });

    const result = await r.json();

    if (!r.ok || !result.ok)
    {
      status.textContent =
        result.error || 'Save failed.';
      return;
    }

    status.textContent =
      'Pool settings saved. Changes take effect after restart.';

    $('poolPassword').value = '';
  }
  catch(e)
  {
    status.textContent = 'Save failed.';
  }
});

$('rebootButton').addEventListener('click', async () =>
{
  const status = $('systemStatus');

  if (!confirm('Are you sure you want to restart the device?'))
    return;

  status.textContent = 'Restarting...';

  try
  {
    const r = await fetch('/api/reboot',
    {
      method: 'POST'
    });

    if (!r.ok)
      throw new Error('reboot failed');

    status.textContent =
      'The device is restarting. Wait a few seconds...';
  }
  catch(e)
  {
    status.textContent =
      'Connection lost. The device is probably restarting.';
  }
});

$('wifiResetButton').addEventListener('click', async () =>
{
  const status = $('systemStatus');

  if (!confirm(
    'Resetting the Wi-Fi settings will disconnect the current network connection.\n\n' +
    'After restart, the device will enter configuration AP mode and Wi-Fi must be configured again.\n\n' +
    'Pool and wallet settings will be preserved.\n\n' +
    'Biztosan folytatod?'
  ))
    return;

  if (!confirm(
    'Final confirmation:\n\n' +
    'Are you sure you want to reset the Wi-Fi settings?'
  ))
    return;

  status.textContent = 'Reset Wi-Fi settings...';

  try
  {
    const r = await fetch('/api/reset-wifi',
    {
      method: 'POST'
    });

    if (!r.ok)
      throw new Error('reset failed');

    status.textContent =
      'Wi-Fi settings reset. The device is restarting in configuration AP mode...';
  }
  catch(e)
  {
    status.textContent =
      'Connection lost. The device is probably restarting in configuration AP mode.';
  }
});

</script>
<div class="watermark">
  Misu's Mining Lab • Hashing on NerdMiner • ESP32 Web Manager
</div>
</body>
</html>
)HTML";

static void startStatsServer()
{
  statsServer.on("/", HTTP_GET, []() {
    statsServer.send_P(200, "text/html", STATS_PAGE);
  });

  statsServer.on("/api/stats", HTTP_GET, []() {
    StaticJsonDocument<768> doc;
    doc["hashrate_khs"] = liveHashrateKHs;
    doc["avg10_khs"] = avg10HashrateKHs;
    doc["avg60_khs"] = avg60HashrateKHs;
    doc["accepted"] = poolAccepted;
    doc["rejected"] = poolRejected;
    doc["shares32"] = shares;
    doc["valid_blocks"] = valids;
    doc["templates"] = templates;
    doc["best_diff"] = best_diff;
    doc["uptime_s"] = upTime;
    doc["rssi_dbm"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    doc["temp_c"] = temperatureRead();
    doc["pool"] = Settings.PoolAddress;
    doc["pool_port"] = Settings.PoolPort;
    doc["ip"] = WiFi.localIP().toString();
    doc["pool_connected"] = client.connected();
    doc["chip_model"] = ESP.getChipModel();
    doc["subscribed"] = isMinerSuscribed;
    String out;
    out.reserve(512);
    serializeJson(doc, out);
    statsServer.send(200, "application/json", out);
  });
statsServer.on("/config", HTTP_GET, []() {
  statsServer.send_P(200, "text/html", CONFIG_PAGE);
});

statsServer.on("/api/config", HTTP_GET, []() {
  StaticJsonDocument<768> doc;

  doc["ssid"] = WiFi.SSID();
  doc["current_bssid"] = WiFi.BSSIDstr();
  doc["current_channel"] = WiFi.channel();
  doc["mac"] = WiFi.macAddress();

  doc["bssid_lock"] = Settings.WifiBSSIDLock;
  doc["saved_bssid"] = Settings.WifiBSSID;
  doc["saved_channel"] = Settings.WifiChannel;
  doc["fallback"] = Settings.WifiFallback;

  doc["pool"] = Settings.PoolAddress;
  doc["pool_port"] = Settings.PoolPort;
  doc["wallet"] = Settings.BtcWallet;

  doc["system_ip"] = WiFi.localIP().toString();
  doc["chip_model"] = ESP.getChipModel();
  doc["chip_revision"] = ESP.getChipRevision();
  doc["cpu_mhz"] = ESP.getCpuFreqMHz();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["sdk_version"] = ESP.getSdkVersion();
  doc["uptime_s"] = upTime;

  String out;
  out.reserve(384);
  serializeJson(doc, out);

  statsServer.send(200, "application/json", out);
});

statsServer.on("/api/wifi-config", HTTP_POST, []() {

  if (!statsServer.hasArg("bssid") ||
      !statsServer.hasArg("channel"))
  {
    statsServer.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"Missing BSSID or channel.\"}"
    );
    return;
  }

  String bssid = statsServer.arg("bssid");
  int channel = statsServer.arg("channel").toInt();

  bool lock =
    statsServer.hasArg("lock") &&
    statsServer.arg("lock") == "1";

  bool fallback =
    statsServer.hasArg("fallback") &&
    statsServer.arg("fallback") == "1";

  bool saved = saveWifiBSSIDConfig(
    bssid.c_str(),
    channel,
    lock,
    fallback
  );

  if (!saved)
  {
    statsServer.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"Invalid data or save error.\"}"
    );
    return;
  }

  statsServer.send(
    200,
    "application/json",
    "{\"ok\":true}"
  );
});

statsServer.on("/api/pool-config", HTTP_POST, []() {

  if (!statsServer.hasArg("host") ||
      !statsServer.hasArg("port") ||
      !statsServer.hasArg("wallet"))
  {
    statsServer.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"Missing pool data.\"}"
    );
    return;
  }

  String host = statsServer.arg("host");
  int port = statsServer.arg("port").toInt();
  String wallet = statsServer.arg("wallet");

  bool changePassword = statsServer.hasArg("password");
  String password = changePassword
    ? statsServer.arg("password")
    : "";

  bool saved = savePoolConfig(
    host.c_str(),
    port,
    wallet.c_str(),
    password.c_str(),
    changePassword
  );

  if (!saved)
  {
    statsServer.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"Invalid pool data or save error.\"}"
    );
    return;
  }

  statsServer.send(
    200,
    "application/json",
    "{\"ok\":true}"
  );
});

statsServer.on("/api/reboot", HTTP_POST, []() {

  statsServer.send(
    200,
    "application/json",
    "{\"ok\":true}"
  );

  delay(500);
  ESP.restart();
});

statsServer.on("/api/reset-wifi", HTTP_POST, []() {

  statsServer.send(
    200,
    "application/json",
    "{\"ok\":true}"
  );

  delay(500);
  reset_wifi_configuration();
});

statsServer.on("/api/wifi-scan", HTTP_GET, []() {
  int16_t scanState = WiFi.scanComplete();

  // -1 = scan folyamatban
  if (scanState == WIFI_SCAN_RUNNING)
  {
    statsServer.send(202, "application/json",
                     "{\"status\":\"scanning\"}");
    return;
  }

  // -2 = no result / scan has not started yet
  if (scanState == WIFI_SCAN_FAILED)
  {
    WiFi.scanDelete();

    // async = true, hidden = false
    WiFi.scanNetworks(true, false);

    statsServer.send(202, "application/json",
                     "{\"status\":\"scanning\"}");
    return;
  }

  DynamicJsonDocument doc(4096);

  doc["status"] = "done";

  JsonArray networks = doc.createNestedArray("networks");

  // Do not allow the JSON document to grow without limit.
  int16_t count = scanState;
  if (count > 20)
    count = 20;

  String currentBSSID = WiFi.BSSIDstr();

  for (int16_t i = 0; i < count; i++)
  {
    JsonObject ap = networks.createNestedObject();

    String bssid = WiFi.BSSIDstr(i);

    ap["ssid"] = WiFi.SSID(i);
    ap["bssid"] = bssid;
    ap["rssi"] = WiFi.RSSI(i);
    ap["channel"] = WiFi.channel(i);
    ap["current"] = bssid.equalsIgnoreCase(currentBSSID);
  }

  String out;
  out.reserve(3000);

  serializeJson(doc, out);

  WiFi.scanDelete();

  statsServer.send(200, "application/json", out);
});

  statsServer.begin();
  statsServerStarted = true;
  Serial.printf("[STATS] Live dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
}

void runMonitor(void *name)
{

  Serial.println("[MONITOR] started");
  restoreStat();

  unsigned long mLastCheck = 0;

  resetToFirstScreen();

  unsigned long frame = 0;

  uint32_t seconds_elapsed = 0;

  totalKHashes = (Mhashes * 1000) + hashes / 1000;
  uint32_t last_update_millis = millis();
  uint32_t uptime_frac = 0;
  float hashrateHistory[60] = {0};
  uint8_t historyPos = 0;
  uint8_t historyCount = 0;
  while (1)
  {
    if (!statsServerStarted && WiFi.status() == WL_CONNECTED) startStatsServer();
    if (statsServerStarted) statsServer.handleClient();

    uint32_t now_millis = millis();
    if (now_millis < last_update_millis)
      now_millis = last_update_millis;

    uint32_t mElapsed = now_millis - mLastCheck;
    if (mElapsed >= 1000)
    {
      mLastCheck = now_millis;
      last_update_millis = now_millis;
      unsigned long currentKHashes = (Mhashes * 1000) + hashes / 1000;
      elapsedKHs = currentKHashes - totalKHashes;
      totalKHashes = currentKHashes;

      // Scale by the actual sample interval instead of assuming exactly 1000 ms.
      liveHashrateKHs = (mElapsed > 0) ? ((float)elapsedKHs * 1000.0f / (float)mElapsed) : 0.0f;
      hashrateHistory[historyPos] = liveHashrateKHs;
      historyPos = (historyPos + 1) % 60;
      if (historyCount < 60) historyCount++;

      float sum60 = 0.0f;
      float sum10 = 0.0f;
      for (uint8_t i = 0; i < historyCount; ++i) sum60 += hashrateHistory[i];
      const uint8_t n10 = (historyCount < 10) ? historyCount : 10;
      for (uint8_t i = 0; i < n10; ++i) {
        int idx = (int)historyPos - 1 - i;
        if (idx < 0) idx += 60;
        sum10 += hashrateHistory[idx];
      }
      avg60HashrateKHs = historyCount ? (sum60 / historyCount) : 0.0f;
      avg10HashrateKHs = n10 ? (sum10 / n10) : 0.0f;

      uptime_frac += mElapsed;
      while (uptime_frac >= 1000)
      {
        uptime_frac -= 1000;
        upTime ++;
      }

      drawCurrentScreen(mElapsed);

      // Monitor state when hashrate is 0.0
      if (elapsedKHs == 0)
      {
        Serial.printf(">>> [i] Miner: newJob>%s / inRun>%s) - Client: connected>%s / subscribed>%s / wificonnected>%s\n",
            "true",//(1) ? "true" : "false",
            isMinerSuscribed ? "true" : "false",
            client.connected() ? "true" : "false", isMinerSuscribed ? "true" : "false", WiFi.status() == WL_CONNECTED ? "true" : "false");
      }

      #ifdef DEBUG_MEMORY
      Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
      Serial.printf("### Max stack usage: %d\n", uxTaskGetStackHighWaterMark(NULL));
      #endif

      seconds_elapsed++;

      if(seconds_elapsed % (saveIntervals[currentIntervalIndex]) == 0){
        saveStat();
        seconds_elapsed = 0;
        if(currentIntervalIndex < saveIntervalsSize - 1)
          currentIntervalIndex++;
      }
    }
    animateCurrentScreen(frame);
    doLedStuff(frame);

    vTaskDelay(DELAY / portTICK_PERIOD_MS);
    frame++;
  }
}
