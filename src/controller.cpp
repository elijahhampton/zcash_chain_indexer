#include "controller.h"

#include <vector>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include "config.h"

Controller::Controller(std::unique_ptr<CustomClient> rpcClientIn, std::unique_ptr<Syncer> syncerIn)
try : rpcClient(rpcClientIn), syncer(syncerIn)
{
    const std::string connection_string =
        "dbname=" + Config::getDatabaseName() +
        " user=" + Config::getDatabaseUser() +
        " password=" + Config::getDatabasePassword() +
        " host=" + Config::getDatabaseHost() +
        " port=" + Config::getDatabasePort();

    // Five connections are assigned to each hardware thread
    size_t poolSize = std::thread::hardware_concurrency() * 5;
    this->database.Connect(poolSize, connection_string)

    if (this->rpcClient == nullptr) {
        throw std::runtime_error("RPC client failed to initialize.");
    }

    if (this->syncer == nullptr) {
        throw std::runtime_error("Syncer failed to initialize.");
    }
}
catch (const std::exception &e)
{
    std::stringstream err_stream;
    err_stream << "Controller uninitialized successfully with error: " << e.what() << std::endl;
    throw std::runtime_error(err_stream.str());
}

Controller::~Controller()
{
    this->Shutdown();
}

void Controller::InitAndSetup()
{
    try
    {
        this->database->CreateTables();
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error("Database failed to create tables.");
    }
}

void Controller::StartSyncLoop()
{
    syncing_thread = std::thread{&Syncer::StartSyncLoop, this->syncer.get()};
}

void Controller::StartSync()
{
    this->syncer->Sync();
}

void Controller::StartMonitoringPeers()
{
    peer_monitoring_thread = std::thread{&Syncer::InvokePeersListRefreshLoop, this->syncer.get()};
}

void Controller::StartMonitoringChainInfo()
{
    chain_info_monitoring_thread = std::thread{&Syncer::InvokeChainInfoRefreshLoop, this->syncer.get()};
}

void Controller::Shutdown()
{
    this->syncer->Stop();
}

void Controller::JoinJoinableSyncingOperations()
{
    if (syncing_thread.joinable())
    {
        syncing_thread.join();
    }

    if (peer_monitoring_thread.joinable())
    {
        peer_monitoring_thread.join();
    }

    if (chain_info_monitoring_thread.joinable())
    {
        chain_info_monitoring_thread.join();
    }
}

int main()
{
    Database database;
    CustomClient rpcClient(Config::getRpcUrl(), Config::getRpcUsername(), Config::getRpcPassword());
    Syncer syncer(rpcClient, database);
    Controller controller(rpcClient, syncer);
    controller.InitAndSetup();
    controller.StartSyncLoop();
    controller.StartMonitoringPeers();
    controller.StartMonitoringChainInfo();
    controller.JoinJoinableSyncingOperations();
    controller.Shutdown();

    return 0;
}