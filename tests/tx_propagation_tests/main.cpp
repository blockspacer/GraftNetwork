// Copyright (c) 2017, The Graft Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2014-2017 The Monero Project

#include "wallet/wallet2_api.h"
#include "cryptonote_basic/account.h"

#include "include_base_utils.h"
#include "cryptonote_config.h"
#include "string_coding.h"
#include "DaemonRpcClient.h"

#include <boost/chrono/chrono.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/tokenizer.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include <iostream>
#include <vector>
#include <atomic>
#include <functional>
#include <string>
#include <chrono>
#include <thread>




namespace po = boost::program_options;
namespace fs = ::boost::filesystem;
using namespace payment_processor;
using namespace Monero;
using namespace std;
using namespace std::chrono;

typedef pair<string, uint64_t> TxRecord;
typedef pair<string, string> Payment;

namespace {

    template <typename T>
    T gen_random(T lower_bound, T upper_bound)
    {
        std::uniform_real_distribution<T> unif(lower_bound,upper_bound);
        std::random_device rd;
        std::default_random_engine re(rd());
        return unif(re);
    }


    bool read_payments(const string &filename, vector<Payment> &payments)
    {
        ifstream strm(filename);

        if (!strm.is_open()) {
            LOG_ERROR("Error opening file " << filename);
            return false;
        }
        LOG_PRINT_L1("reading payments from file: " << filename);

        string line;
        while (getline(strm, line)) {
            typedef boost::tokenizer<boost::char_separator<char>> tokenizer;
            vector<string> tokens;
            boost::char_separator<char> sep{","};
            tokenizer tok{line, sep};
            for (const auto &t : tok)
                tokens.push_back(t);

            if (tokens.size() != 2) {
                LOG_ERROR("Error parsing payment: " << line);
                return false;
            }
            Payment p = make_pair(tokens[0], tokens[1]);
            payments.push_back(p);
        }

        return true;
    }



    // return the filenames of all files that have the specified extension
    // in the specified directory and all subdirectories
    void get_all(const fs::path& root, const string& ext, vector<fs::path>& ret)
    {

        LOG_PRINT_L2("Checking for transactions files (*." << ext << ") in " << root);
        if(!fs::exists(root) || !fs::is_directory(root))  {
            LOG_ERROR("Path " << root.string() << " is not exists nor directory..");
            return;
        }

        fs::recursive_directory_iterator it(root);
        fs::recursive_directory_iterator endit;

        while (it != endit) {
            LOG_PRINT_L2("checking file " << it->path());
            LOG_PRINT_L2("ext: " << it->path().extension());
            if (fs::is_regular_file(*it) && it->path().extension() == ext) {
                LOG_PRINT_L2("file " << it->path() << " is tx file");
                ret.push_back(it->path().filename());
            }
            ++it;
        }

    }
}

Monero::Wallet * open_wallet_helper(const std::string &wallet_path, const std::string &wallet_password, const std::string &daemon_address,
                                    bool testnet)
{
    Monero::WalletManager *wmgr = Monero::WalletManagerFactory::getWalletManager();
    Monero::Wallet   * w = wmgr->openWallet(wallet_path, wallet_password, testnet);

    if (w->status() != Wallet::Status_Ok) {
        LOG_ERROR("Error opening wallet " << wallet_path << ", " << w->errorString());
        delete w;
        return nullptr;
    }
    if (!w->init(daemon_address, 0)) {
        LOG_ERROR("Error connecting to daemon at " << daemon_address << ", :" << w->errorString());
        delete w;
        return nullptr;
    }

    w->refresh();

    return w;
}

/*!
 * \brief generate_transactions - generates transactions and stores them to disk
 * \param num                   - number of transactions to generate
 * \param out_txes              - vector of tx ids generated
 * \param output_dir            - directory where transaction files will be written. filename will be <tx_id>.gtx
 * \return                      - true on success
 */
bool generate_transactions(const std::string &wallet_path, const std::string &wallet_password, const std::string &daemon_address,
                           bool testnet, size_t num,
                           const std::string &input_file,
                           std::vector<std::string> &out_txes, const std::string &output_dir)
{

    if (num == 0)
        return true;

    auto gen_tx = [&](Wallet * w, const string &address, uint64_t amount) {
        PendingTransaction * ptx = w->createTransaction(address, "", amount, 4);

        if (ptx->txCount() > 1) {
            LOG_PRINT_L0("transfer splitted, ignoring");
        } else {
            if (ptx->status() == PendingTransaction::Status_Ok) {
                string unsigned_filename = output_dir + "/" + ptx->txid()[0] + ".gtx.unsigned";
                if (ptx->commit(unsigned_filename)) {
                    // sign tx;
                    UnsignedTransaction * utx = w->loadUnsignedTx(unsigned_filename);
                    vector<string> tx_ids;
                    string signed_filename_tmp = output_dir + "/" + ptx->txid()[0] + ".gtx";
                    if (!utx->sign(signed_filename_tmp, tx_ids)) {
                        LOG_ERROR("Error signing tx " << utx->errorString() );
                        return false; // memleak, but doesn't really care here
                    }
                    delete utx;
                    string signed_filename = output_dir + "/" + tx_ids[0] + ".gtx";
                    boost::filesystem::rename(signed_filename_tmp, signed_filename);
                    boost::filesystem::remove(unsigned_filename);
                    out_txes.push_back(tx_ids[0]);

                } else {
                    LOG_ERROR("Error saving tx to file: " << ptx->errorString());
                }
            }
        }
        w->disposeTransaction(ptx);
        return true;
    };

    LOG_PRINT_L1("generating transactions");
    // TODO: smart_ptr
    Monero::Wallet * w  = open_wallet_helper(wallet_path, wallet_password, daemon_address, testnet);
    if (!w)
        return false;

    // read payments from file, if passed
    if (!input_file.empty()) {
        vector<Payment> payments;
        if (!read_payments(input_file, payments)) {
            LOG_ERROR("Error reading payments from " << input_file);
            return false;
        }
        for (const auto &p : payments) {
            if (!gen_tx(w, p.first, Wallet::amountFromString(p.second))) {
                return false;
            }
        }
    } else {
        // otherwise, generate random addresses
        for (int i = 0; i < num; ++i) {
            cryptonote::account_base account;
            account.generate();
            double amount = gen_random(0.1, 1.0);
            string address = account.get_public_address_str(testnet);
            LOG_PRINT_L0("Sending " << amount << " to " << address);
            if (!gen_tx(w, address, Wallet::amountFromDouble(amount)))
                return false;
        }
    }

    w->store("");
    Monero::WalletManagerFactory::getWalletManager()->closeWallet(w);

    return true;
}

/*!
 * \brief send_transactions - sends pre-generated transactions from input dir to the pool
 * \param input_dir         - input dir with transaction files. (*.gtx)
 * \param output_timestamps - vector of pairs <tx_id, timestamp> - where
 * \return
 */
bool send_transactions(const std::string &wallet_path, const std::string &wallet_password, const std::string &daemon_address,
                       bool testnet, const string &input_dir, vector<TxRecord> &output_timestamps)
{
    LOG_PRINT_L1("sending transactions");

    Monero::Wallet * w  = open_wallet_helper(wallet_path, wallet_password, daemon_address, testnet);
    if (!w) {
        LOG_ERROR("Error opening wallet " << wallet_path);
        return false;
    }

    // get all tx files in one vector
    vector<fs::path> tx_files;
    get_all(input_dir, ".gtx", tx_files);



    for (const fs::path &tx_file : tx_files) {
//        UnsignedTransaction * utx = w->loadUnsignedTx(tx_file.string());
//        if (utx->status() != UnsignedTransaction::Status_Ok) {
//            LOG_ERROR("Error loading unsigned transaction from " << tx_file.string() << ", " << utx->errorString());
//            continue;
//        }
//        string signed_tx_filename = tx_file.string() + ".signed";
//        // TODO: add "tx_id" method to unsigned tx;
//        utx->sign(signed_tx_filename);
        LOG_PRINT_L1("Sending tx: " << tx_file.filename().string());
        w->submitTransaction(tx_file.filename().string());

        milliseconds ms = duration_cast< milliseconds >(system_clock::now().time_since_epoch());
        // filename is the tx_hash, but normally there should be method in UnsignedTransaction;
        TxRecord tx_record = make_pair(tx_file.stem().string(), ms.count());
        fs::remove(tx_file);
        output_timestamps.push_back(tx_record);

    }
    w->store("");
    Monero::WalletManagerFactory::getWalletManager()->closeWallet(w);

    return true;
}

/*!
 * \brief monitor_transactions - waits for transactions to be placed into tx pool.
 *                               once tx found in pool, it added to output vector 'result' with the timestamp when it found
 * \param txs_to_wait          - vector of txes to wait for
 * \param timeout_s            - wait no longer than timeout_s
 * \param result               - pairs <tx_id, timestamp> will be written here
 * \return                     - true on success
 */
bool monitor_transactions(const std::string &daemon_address, const vector<string> &txs_to_wait, size_t timeout_s,
                          vector<TxRecord> &result)
{
    LOG_PRINT_L1("waiting for transactions");

    DaemonRpcClient rpcClient(daemon_address, "", "");
    vector<string> txs_to_wait_local = txs_to_wait;
    std::chrono::time_point<std::chrono::system_clock> start, end;
    start = std::chrono::system_clock::now();

    while (result.size() < txs_to_wait.size()) {
        for (auto it = txs_to_wait_local.begin(); it != txs_to_wait_local.end(); ++it) {
            cryptonote::transaction unused;
            end = std::chrono::system_clock::now();
            size_t elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
            if (elapsed_seconds >= timeout_s) {
                LOG_PRINT_L0("timeout: " << timeout_s << " passed, quiting..");
                return false;
            }
            bool res = rpcClient.get_tx_from_pool(*it, unused);
            if (res) {
                TxRecord txr;
                txr.first = *it;
                txr.second = duration_cast< milliseconds >(system_clock::now().time_since_epoch()).count();
                result.push_back(txr);
                txs_to_wait_local.erase(it);
                break;
            }
        }

    }
    return true;
}


bool write_tx_records_to_file(const vector<TxRecord> &txrs, const string &filename)
{
    LOG_PRINT_L1("Saving tx records to a file " << filename);
    ofstream strm(filename);
    if (!strm.is_open()) {
        LOG_ERROR("Error opening file " << filename);
        return false;
    }
    for (const TxRecord & txr : txrs) {
        strm << txr.first << "," << txr.second << endl;
    }

    return true;
}

bool read_tx_hashes_from_file(vector<string> &txs, const std::string &filename)
{
    ifstream strm(filename);

    if (!strm.is_open()) {
        LOG_ERROR("Error opening file " << filename);
        return false;
    }
    LOG_PRINT_L1("reading txs from file: " << filename);

    string line;
    while (getline(strm, line)) {
        txs.push_back(line);
    }

    return true;
}

bool write_tx_hashes_to_file(const vector<string> &txs, const std::string &filename)
{
    ofstream strm(filename);
    if (!strm.is_open()) {
        LOG_ERROR("Error opening file " << filename);
        return false;
    }

    for (const string &tx: txs) {
        strm << tx << endl;
    }
    return true;
}

static const char * HELP_MSG =
 "CLI interface: tx-test <command>"
 "\nwhere  <command> is one of the following:"
 "\n\t\"generate\" - generates transactions and saves them to directory passed in --output-dir option"
 "\n\t\"send\"     - sends transactions and saves result (file where each line is <tx_id>:<timestamp>) into file passed as --output-file arg"
 "\n\t\"monitor\"  - monitoring transaction pool for the transactions passed in file which name is passed as --input-file arg"
 "\n\t "
 "\n\t    --wallet-path=<wallet_path>, all commands"
 "\n\t     --wallet-password=<wallet_password>, all commands"
 "\n\t     --daemon-address=<daemon-address>, only monitor and send commands"
 "\n\t     --output-dir=<directory where to write transaction files>, only for \"generate\" command"
 "\n\t     --input-dir=<directory where to read transaction files>,  only for \"send\" command"
 "\n\t     --output-file=<file where report will be written to>, for  \"send\" and \"monitor\" commands"
 "\n\t     --input-file=<file where monitor will read transaciton ids from>, only for \"monitor\" command.\n";



int main(int argc, char** argv)
{
    epee::string_tools::set_module_name_and_folder(argv[0]);


    string command;
    string wallet_path;
    string wallet_password;
    string daemon_address;
    string output_dir;
    string input_dir;
    string output_file;
    string input_file;
    int    log_level;
    int    tx_to_gen_count = 1;
    size_t monitor_timeout = 30;
    bool   testnet;

    try {
        po::options_description desc("Usage: tx_tests <command> [options]"
                                     "\n\twhere command one of the 'generate' | 'send' | 'monitor'"
                                     "\n\tAllowed options:");
        desc.add_options()
                ("help", "produce help message")
                ("command", po::value<string>(&command), "command to execute")
                ("wallet-path",     po::value<string>(), "path to wallet file")
                ("wallet-password", po::value<string>(), "wallet password")
                ("daemon-address",  po::value<string>(), "daemon-address")
                ("output-dir",      po::value<string>(), "output directory")
                ("count",           po::value<int>(&tx_to_gen_count)->default_value(1), "number of transactions to generate")
                ("input-dir",       po::value<string>(), "input directory")
                ("output-file",     po::value<string>(), "output file")
                ("input-file",      po::value<string>(), "input file")
                ("timeout",         po::value<size_t>(&monitor_timeout)->default_value(30), "timeout in seconds to wait for transactions while monitoring")
                ("log-level",       po::value<int>(&log_level)->default_value(2), "log-level")
                ("testnet",         po::value<bool>(&testnet)->default_value(true), "testnet");

        po::positional_options_description p;
        p.add("command", 1);
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);

        po::notify(vm);

        if (vm.count("help") || vm.count("command") == 0) {
            cout << desc << "\n";
            return 0;
        }

        command = vm["command"].as<string>();
        std::cout << "command: " << command << endl;

        if (!(command == "generate" || command == "send" || command == "monitor")) {
            cout << "unknown command: " << command << endl;
            cout << desc << "\n";
            return 0;
        }


        mlog_configure("", true);
        mlog_set_log_level(log_level);

        if (vm.count("daemon-address") == 0) {
            cout << "Daemon address is missing for " << command << " command\n" ;
            cout << desc << "\n";
            return -1;
        }

        daemon_address = vm["daemon-address"].as<string>();


        if (command == "generate" || command == "send") {
            if (vm.count("wallet-path") == 0) {
                cout << "Wallet path is missing for " << command << " command";
                cout << desc << "\n";
                return -1;
            }

            wallet_path = vm["wallet-path"].as<string>();
            if (vm.count("wallet-password")) {
                wallet_password = vm["wallet-password"].as<string>();
            }



            // handle generate
            if (command == "generate") {

                if (vm.count("output-dir") == 0) {
                    cout << "output dir is missing for " << command << " command";
                    cout << desc << "\n";
                    return -1;
                }

                output_dir = vm["output-dir"].as<string>();
                if (vm.count("output-file") == 0) {
                    cout << "output file is missing for " << command << " command";
                    cout << desc << "\n";
                    return -1;
                }
                output_file = vm["output-file"].as<string>();

                if (vm.count("input-file")) {
                    input_file = vm["input-file"].as<string>();
                }

                vector<string> tx_hashes;


                if (!generate_transactions(wallet_path, wallet_password, daemon_address,
                                      testnet,
                                      tx_to_gen_count,
                                      input_file,
                                      tx_hashes,
                                      output_dir)) {
                    LOG_ERROR("Error generating transactions");
                    return -1;
                }

                if (!write_tx_hashes_to_file(tx_hashes, output_file)) {
                    LOG_ERROR("Error saving tx hashes");
                    return -1;
                }


            } else if (command == "send") {
                if (vm.count("input-dir") == 0) {
                    cout << "input dir is missing for " << command << " command";
                    cout << desc << "\n";
                    return -1;
                }
                input_dir = vm["input-dir"].as<string>();

                if (vm.count("output-file") == 0) {
                    cout << "output file is missing for " << command << " command";
                    cout << desc << "\n";
                    return -1;
                }
                output_file = vm["output-file"].as<string>();
                vector<TxRecord> out_txs;

                if (!send_transactions(wallet_path, wallet_password, daemon_address, testnet, input_dir, out_txs)) {
                    LOG_ERROR("Error sending transactions...");
                    return -1;
                }
                if (!write_tx_records_to_file(out_txs, output_file)) {
                    LOG_ERROR("Error saving transaction records");
                    return -1;
                }
            }
        } else { // "monitor" here
            if (vm.count("input-file") == 0) {
                cout << "Input file is missing for " << command << " command";
                cout << desc << "\n";
                return -1;
            }
            input_file = vm["input-file"].as<string>();

            if (vm.count("output-file") == 0) {
                cout << "output file is missing for " << command << " command";
                cout << desc << "\n";
                return -1;
            }
            output_file = vm["output-file"].as<string>();
            vector<string> tx_hashes;

            if (!read_tx_hashes_from_file(tx_hashes, input_file)) {
                LOG_ERROR("Error reading transactions from file " << input_file);
                return -1;
            }
            vector<TxRecord> tx_records;
            LOG_PRINT_L0("waiting for " << tx_hashes.size() << " transactions");
            if (!monitor_transactions(daemon_address, tx_hashes, monitor_timeout, tx_records)) {
                LOG_ERROR("Error monitor transactions ");
                return -1;
            }

            if (!write_tx_records_to_file(tx_records, output_file)) {
                LOG_ERROR("Error saving transaction records");
                return -1;
            }
        }
    }

    catch(exception& e) {
        cerr << "error: " << e.what() << "\n";
        return 1;
    }
    catch(...) {
        cerr << "Exception of unknown type!\n";
    }

    return 0;
}



