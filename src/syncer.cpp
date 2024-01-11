#include "syncer.h"
#include <algorithm>
#include "httpclient.h"
#include <iostream>
#include <string>
#include <optional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <chrono>
#include <vector>
#include <sstream>
#include <boost/process.hpp>
#include <fstream>
#include <queue>
#include "config.h"

size_t Syncer::CHUNK_SIZE = std::stoi(Config::getBlockChunkProcessingSize());
const uint8_t Syncer::MAX_CONCURRENT_THREADS = std::thread::hardware_concurrency();

Syncer::Syncer(CustomClient &httpClientIn, Database &databaseIn) : httpClient(httpClientIn), database(databaseIn), latestBlockSynced{0}, latestBlockCount{0}, isSyncing{false}
{
}

Syncer::~Syncer() {}

void Syncer::CheckAndDeleteJoinableProcessingThreads(std::vector<std::thread> &processingThreads)
{
    processingThreads.erase(std::remove_if(processingThreads.begin(), processingThreads.end(),
                                           [](const std::thread &t)
                                           { return !t.joinable(); }),
                            processingThreads.end());
}

void Syncer::DoConcurrentSyncOnChunk(const std::vector<size_t> &chunkToProcess)
{

    std::vector<Json::Value> downloadedBlocks;
    this->DownloadBlocksFromHeights(downloadedBlocks, chunkToProcess);

    // Create processable chunk

    std::vector<Json::Value> chunk(downloadedBlocks.begin(), downloadedBlocks.end());

    // Launch a new thread for the current chunk
    std::vector<std::thread> processingThreads;
    processingThreads.emplace_back(
        [this](bool a, const std::vector<Json::Value> &b, uint64_t c, uint64_t d, uint64_t e)
        {
            this->database.StoreChunk(a, b, c, d, e);
        },
        false, chunk, Database::InvalidHeight, Database::InvalidHeight, Database::InvalidHeight);

    // All blocks have been synced since last sync() call.
    // Wait for all remaining threads to complete
    for (auto &thread : processingThreads)
    {
        std::cout << "Processing threads size: " << processingThreads.size() << std::endl;
        std::cout << "waiting for thread to finish" << std::endl;
        if (thread.joinable())
        {
            std::cout << "Found a joinable thread" << std::endl;
            thread.join();
        }
    }

    // Erase the rest of the processing threads
    this->CheckAndDeleteJoinableProcessingThreads(processingThreads);

    std::cout << "Processing threads size: " << processingThreads.size() << std::endl;
    // Make sure no threads exist
    if (processingThreads.size() > 0)
    {
        throw std::runtime_error("Dangling processing threads still running...");
    }
}

void Syncer::DoConcurrentSyncOnRange(bool isTrackingCheckpointForChunks, uint64_t start, uint64_t end)
{
    std::cout << "DoConcurrentSyncOnRange(uint start, uint end)"
              << "(" << start << "," << end << ")" << std::endl;
    std::vector<std::thread> processingThreads;
    std::vector<Json::Value> downloadedBlocks;

    // Generate a checkpoint for the start point if found
    std::optional<Database::Checkpoint> checkpointOpt = this->database.GetCheckpoint(start);
    bool checkpointExist = checkpointOpt.has_value();
    Database::Checkpoint checkpoint;
    size_t chunkStartPoint{start};
    size_t chunkEndPoint;

    if (end - chunkStartPoint + 1 >= Syncer::CHUNK_SIZE)
    {
        chunkEndPoint = chunkStartPoint + Syncer::CHUNK_SIZE - 1;
    }
    else
    {
        chunkEndPoint = end;
    }

    if (checkpointExist)
    {
        checkpoint = checkpointOpt.value();

        // Set chunkStartPoint to the next block after the last check point
        chunkStartPoint = checkpoint.lastCheckpoint;
    }

    std::cout << "Syncing by range" << std::endl;
    std::cout << "Sync start: " << chunkStartPoint << std::endl;
    std::cout << "Sync End: " << chunkEndPoint << std::endl;

    bool isExistingCheckpoint = false;
    while (chunkStartPoint <= end)
    {
        if (!checkpointExist && isTrackingCheckpointForChunks)
        {
            isExistingCheckpoint = false;
            this->database.CreateCheckpointIfNonExistent(chunkStartPoint, chunkEndPoint);
        }
        else
        {
            isExistingCheckpoint = true;
        }

        // Download blocks
        this->DownloadBlocks(downloadedBlocks, chunkStartPoint, chunkEndPoint);

        // Create processable chunk
        std::vector<Json::Value> chunk(downloadedBlocks.begin(), downloadedBlocks.end());

        // Max number of threads have been met
        while (processingThreads.size() >= Syncer::MAX_CONCURRENT_THREADS)
        {
            bool joinedAtLeastOneThread = false;

            while (!joinedAtLeastOneThread)
            {
                for (auto &thread : processingThreads)
                {
                    if (thread.joinable())
                    {
                        thread.join();
                        joinedAtLeastOneThread = true;
                        break; // Exit the for loop as soon as one thread is joined
                    }
                }

                // If no joinable thread was found, wait for the specified cool-off time
                if (!joinedAtLeastOneThread)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(Syncer::JOINABLE_THREAD_COOL_OFF_TIME_IN_SECONDS));
                }
            }

            this->CheckAndDeleteJoinableProcessingThreads(processingThreads);
        }

        // Launch a new thread for the current chunk
        std::cout << "Processing new chunk of blocks starting at range: " << chunkStartPoint << std::endl;
        processingThreads.emplace_back(
            [this](bool a, const std::vector<Json::Value> &b, uint64_t c, uint64_t d, uint64_t e)
            {
                this->database.StoreChunk(a, b, c, d, e);
            },
            isTrackingCheckpointForChunks, chunk, chunkStartPoint, chunkEndPoint, isExistingCheckpoint ? start : chunkStartPoint);

        // All blocks processed
        downloadedBlocks.clear();

        // Update chunkStartPoint to the next chunk
        chunkStartPoint = chunkEndPoint + 1;

        // Update chunkEndPoint for the next chunk, capped at 'end'
        if (end - chunkStartPoint + 1 >= Syncer::CHUNK_SIZE)
        {
            chunkEndPoint = chunkStartPoint + Syncer::CHUNK_SIZE - 1;
        }
        else
        {
            chunkEndPoint = end;
        }

        std::cout << "StartPoint updated to: " << chunkStartPoint << std::endl;
        std::cout << "EndPoint updated to: " << chunkEndPoint << std::endl;
    }

    // All blocks have been synced since last sync() call.
    // Wait for all remaining threads to finish to complete the syncing operation.
    this->JoinAndWaitForAllThreadsToFinish(processingThreads);

    // Erase the rest of the processing threads
    this->CheckAndDeleteJoinableProcessingThreads(processingThreads);

    // Make sure no threads exist
    if (processingThreads.size() > 0)
    {
        throw std::runtime_error("Dangling processing threads still running...");
    }
}

void Syncer::JoinAndWaitForAllThreadsToFinish(std::vector<std::thread> &threads)
{
    for (auto &thread : threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

void Syncer::StartSyncLoop()
{
    const std::chrono::minutes syncInterval(60);

    while (this->run_syncing)
    {
        std::lock_guard<std::mutex> syncLock(cs_sync);

        bool shouldSync = this->ShouldSyncWallet();

        if (shouldSync)
        {
            this->Sync();
        }

        std::this_thread::sleep_for(syncInterval);
    }
}

void Syncer::InvokePeersListRefreshLoop() noexcept
{
    while (this->run_peer_monitoring)
    {
        std::lock_guard<std::mutex> lock(httpClientMutex);
        Json::Value peer_info = this->httpClient.getpeerinfo();
        this->database.StorePeers(peer_info);

        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}

void Syncer::InvokeChainInfoRefreshLoop() noexcept
{
    while (this->run_chain_info_monitoring)
    {
        std::lock_guard<std::mutex> lock(httpClientMutex);
        Json::Value chain_info = this->httpClient.getblockchaininfo();
        this->database.StoreChainInfo(chain_info);

        std::this_thread::sleep_for(std::chrono::minutes(30));
    }
}

void Syncer::SyncUnfinishedCheckpoints()
{
    std::stack<Database::Checkpoint> checkpoints = this->database.GetUnfinishedCheckpoints();
    Database::Checkpoint currentCheckpoint;

    std::cout << "Checkpoints to complete: " << checkpoints.size() << std::endl;
    while (checkpoints.size() > 0)
    {
        currentCheckpoint = checkpoints.top();
        std::cout << "Processing a checkpoint by range: "
                  << "(" << currentCheckpoint.chunkStartHeight << "," << currentCheckpoint.chunkEndHeight << "," << currentCheckpoint.lastCheckpoint << ")" << std::endl;
        checkpoints.pop();

        // Sync the checkpoint by the range of its start to end point
        this->DoConcurrentSyncOnRange(true, currentCheckpoint.chunkStartHeight, currentCheckpoint.chunkEndHeight);
    }
}

void Syncer::Sync()
{
    try
    {
        this->isSyncing = true;

        // TODO: Sync missed blocks
        // std::list<std::uint64_t> missedBlocks = this->database.GetMissedBlocks();
        // this->SyncMissedBlocks(missedBlocks, SyncDirection::Reverse);

        // Sync unfinished checkpoints
        this->SyncUnfinishedCheckpoints();

        // Sync new blocks
        this->LoadTotalBlockCountFromChain();
        this->LoadSyncedBlockCountFromDB();
        uint64_t numNewBlocks = this->latestBlockCount - this->latestBlockSynced;

        if (numNewBlocks == 0)
        {
            std::cout << "Syncing path: No new blocks found mined." << std::endl;
            this->isSyncing = false;
            return;
        }
        else if (numNewBlocks >= CHUNK_SIZE)
        {
            std::cout << "Syncing path: Syncing by range." << std::endl;
            uint startRangeChunk = this->latestBlockSynced == 0 ? this->latestBlockSynced : this->latestBlockSynced + 1;

            this->DoConcurrentSyncOnRange(true, startRangeChunk, this->latestBlockCount);
        }
        else
        {
            std::cout << "Syncing path: Syncing by chunk" << std::endl;
            // for loop from latest block synced to latest block count
            // Sync until latestBlockCOunt + 1 to include the latest
            std::vector<size_t> heights;
            for (size_t i = this->latestBlockSynced + 1; i < this->latestBlockCount + 1; i++)
            {
                // add to vector each height
                heights.push_back(i);
            }

            this->DoConcurrentSyncOnChunk(heights);
        }

        this->isSyncing = false;
        this->LoadSyncedBlockCountFromDB();

        std::cout << "Syncing complete!\n";
    }
    catch (std::exception &e)
    {
        std::cout << e.what() << std::endl;
    }
}

void Syncer::DownloadBlocksFromHeights(std::vector<Json::Value> &downloadedBlocks, std::vector<size_t> heightsToDownload)
{
    if (heightsToDownload.size() > Syncer::CHUNK_SIZE)
    {
        throw std::runtime_error("Only allowed to download CHUNK_SIZE blocks at a time.");
    }

    std::cout << "Downloading blocks from heights of size: " << heightsToDownload.size() << std::endl;
    Json::Value getblockParams;
    Json::Value blockResultSerialized;

    std::lock_guard<std::mutex> lock(httpClientMutex);
    size_t i{0};

    std::cout << "Size of heights to download: " << std::endl;
    while (i < heightsToDownload.size())
    {
        getblockParams.append(Json::Value(std::to_string(heightsToDownload.at(i))));
        getblockParams.append(Json::Value(2));

        try
        {
            blockResultSerialized = httpClient.CallMethod("getblock", getblockParams);

            if (blockResultSerialized.isNull())
            {
                ++i;
                continue;
            }

            downloadedBlocks.push_back(blockResultSerialized);
            blockResultSerialized.clear();
            getblockParams.clear();
        }
        catch (jsonrpc::JsonRpcException &e)
        {
            std::cout << "Block height: " << i << std::endl;
            std::cout << e.what() << std::endl;
            ++i;
            getblockParams.clear();
            continue;
        }
        catch (std::exception &e)
        {
            std::cout << "Block height: " << i << std::endl;
            std::cout << e.what() << std::endl;
            ++i;
            getblockParams.clear();
            continue;
        }

        ++i;
    }
}

void Syncer::DownloadBlocks(std::vector<Json::Value> &downloadBlocks, uint64_t startRange, uint64_t endRange)
{
    std::cout << "Downloading blocks starting at " << startRange << " and ending at " << endRange << std::endl;

    std::lock_guard<std::mutex> lock(httpClientMutex);
    Json::Value getblockParams{Json::nullValue};
    Json::Value blockResultSerialized{Json::nullValue};

    while (startRange <= endRange)
    {
        getblockParams.append(Json::Value(std::to_string(startRange)));
        getblockParams.append(Json::Value(Syncer::BLOCK_DOWNLOAD_VERBOSE_LEVEL));

        try
        {
            blockResultSerialized = httpClient.CallMethod("getblock", getblockParams);

            if (blockResultSerialized.isNull())
            {
                throw new std::exception();
            }

            downloadBlocks.push_back(blockResultSerialized);
        }
        catch (jsonrpc::JsonRpcException &e)
        {
            this->database.AddMissedBlock(startRange);
            downloadBlocks.push_back(Json::nullValue);
        }
        catch (std::exception &e)
        {
            this->database.AddMissedBlock(startRange);
            downloadBlocks.push_back(Json::nullValue);
        }

        blockResultSerialized.clear();
        getblockParams.clear();
        startRange++;
        std::cout << "Current block download: " << startRange << std::endl;
    }
}

void Syncer::LoadSyncedBlockCountFromDB()
{
    std::cout << "LoadSyncedBlockCountFromDB()" << std::endl;
    this->latestBlockSynced = this->database.GetSyncedBlockCountFromDB();
    std::cout << "Latest block synced: " << this->latestBlockSynced << std::endl;
}

void Syncer::LoadTotalBlockCountFromChain()
{
    std::cout << "LoadTotalBlockCountFromChain()" << std::endl;
    try
    {
        std::lock_guard<std::mutex> lock(httpClientMutex);
        this->latestBlockCount = httpClient.getblockcount().asLargestUInt();
        std::cout << "Most recent chain block count: " << this->latestBlockCount << std::endl;
    }
    catch (jsonrpc::JsonRpcException &e)
    {
        if (std::string(e.what()).find("Loading block index") != std::string::npos)
        {
            while (std::string(e.what()).find("Loading block index") != std::string::npos && std::string(e.what()).find("Verifying blocks") != std::string::npos)
            {
                std::cout << "Loading block index." << std::endl;
            }

            this->LoadTotalBlockCountFromChain();
        }
    }
    catch (std::exception &e)
    {
        std::cout << e.what() << std::endl;
        exit(1);
    }
}

bool Syncer::ShouldSyncWallet()
{
    if (this->isSyncing)
    {
        std::cout << "Program already syncing." << std::endl;
        return false;
    }

    this->LoadTotalBlockCountFromChain();
    this->LoadSyncedBlockCountFromDB();

    if (!this->isSyncing && (this->latestBlockSynced < this->latestBlockCount))
    {
        std::cout << "Sync required.." << std::endl;
        return true;
    }

    std::cout << "No sync is required." << std::endl;
    return false;
}

bool Syncer::GetSyncingStatus() const
{
    return this->isSyncing;
}

void Syncer::StopPeerMonitoring()
{
    this->run_peer_monitoring = false;
}

void Syncer::StopSyncing()
{
    this->run_syncing = false;
}

void Syncer::Stop()
{
    this->StopPeerMonitoring();
    this->StopSyncing();
}