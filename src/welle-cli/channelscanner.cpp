/*
 *    Copyright (C) 2024
 *    welle.io contributors
 *
 *    This file is part of the welle.io.
 *    Many of the ideas as implemented in welle.io are derived from
 *    other work, made available through the GNU general Public License.
 *    All copyrights of the original authors are recognized.
 *
 *    welle.io is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    welle.io is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with welle.io; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "welle-cli/channelscanner.h"
#include "backend/radio-receiver.h"
#include "various/channels.h"
#include "libs/json.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

using namespace std;
using namespace nlohmann;
using namespace chrono;

/* ---------- RadioControllerInterface callbacks ---------- */

void ChannelScanner::onSNR(float snr)
{
    lock_guard<mutex> lock(mtx);
    current_snr = snr;
}

void ChannelScanner::onSyncChange(char isSync)
{
    lock_guard<mutex> lock(mtx);
    synced = (isSync != 0);
    cv.notify_all();
}

void ChannelScanner::onSignalPresence(bool isSignal)
{
    lock_guard<mutex> lock(mtx);
    signal_present = isSignal;
    cv.notify_all();
}

void ChannelScanner::onServiceDetected(uint32_t sId)
{
    lock_guard<mutex> lock(mtx);
    detected_sids.insert(sId);
}

void ChannelScanner::onNewEnsemble(uint16_t eId)
{
    lock_guard<mutex> lock(mtx);
    current_eid = eId;
}

void ChannelScanner::onSetEnsembleLabel(DabLabel& label)
{
    lock_guard<mutex> lock(mtx);
    current_label = label.utf8_label();
}

/* ---------- Scanning logic ---------- */

void ChannelScanner::run(InputInterface& input,
                         const RadioReceiverOptions& rro,
                         int timeout_per_channel_sec)
{
    Channels channels;
    string channelName = Channels::firstChannel;
    int total   = NUMBEROFCHANNELS;
    int scanned = 0;

    while (!channelName.empty()) {
        int freq = channels.getFrequency(channelName);
        scanned++;
        cerr << "[" << scanned << "/" << total << "] "
             << channelName << "  ("
             << fixed << setprecision(3) << freq / 1.0e6 << " MHz) ... "
             << flush;

        /* Reset per-channel state */
        {
            lock_guard<mutex> lock(mtx);
            signal_present = false;
            synced         = false;
            current_snr    = 0.0f;
            current_eid    = 0;
            current_label.clear();
            detected_sids.clear();
        }

        input.setFrequency(freq);
        input.reset();

        /* Give the AGC (hardware or software) time to settle after retuning.
         * Without this delay, auto-gain receivers may not have adjusted their
         * gain before signal detection starts, causing missed channels. */
        this_thread::sleep_for(milliseconds(500));

        /* Phase 1: scan mode – quickly detect signal presence */
        bool has_signal = false;
        {
            RadioReceiver rx(*this, input, rro);
            rx.restart(true); /* doScan=true fires onSignalPresence quickly */

            auto deadline = steady_clock::now() + seconds(3);
            {
                unique_lock<mutex> lock(mtx);
                cv.wait_until(lock, deadline,
                              [this] { return signal_present; });
                has_signal = signal_present;
            }
            rx.stop();
        }

        if (!has_signal) {
            cerr << "no signal" << endl;
            channelName = channels.getNextChannel();
            continue;
        }

        /* Phase 2: full receive – wait for sync and collect services */
        {
            /* Reset sync flag before restarting in full-receive mode */
            {
                lock_guard<mutex> lock(mtx);
                synced = false;
            }

            input.reset();
            RadioReceiver rx(*this, input, rro);
            rx.restart(false);

            /* Wait for sync */
            auto sync_deadline = steady_clock::now() +
                                 seconds(timeout_per_channel_sec);
            {
                unique_lock<mutex> lock(mtx);
                cv.wait_until(lock, sync_deadline,
                              [this] { return synced; });
            }

            bool got_sync;
            {
                lock_guard<mutex> lock(mtx);
                got_sync = synced;
            }

            if (!got_sync) {
                cerr << "signal but no sync" << endl;
                rx.stop();
                channelName = channels.getNextChannel();
                continue;
            }

            /* Give the FIB processor time to accumulate the service list */
            this_thread::sleep_for(seconds(3));

            /* Collect results */
            auto serviceList = rx.getServiceList();

            ScanResult result;
            {
                lock_guard<mutex> lock(mtx);
                result.channel      = channelName;
                result.frequency_hz = static_cast<uint32_t>(freq);
                result.ensemble_id  = current_eid;
                result.ensemble_label = current_label;
                /* Trim trailing whitespace from ensemble label */
                result.ensemble_label.erase(
                    find_if(result.ensemble_label.rbegin(),
                            result.ensemble_label.rend(),
                            [](int c) { return !isspace(c); }).base(),
                    result.ensemble_label.end());
                result.snr = current_snr;
            }

            for (const auto& s : serviceList) {
                ServiceInfo si;
                si.sid         = s.serviceId;
                si.label       = s.serviceLabel.utf8_label();
                si.bitrate_kbps = 0;

                /* Trim trailing whitespace */
                si.label.erase(
                    find_if(si.label.rbegin(), si.label.rend(),
                            [](int c) { return !isspace(c); }).base(),
                    si.label.end());

                /* Bitrate from the first subchannel with a valid ID */
                for (const auto& sc : rx.getComponents(s)) {
                    const auto& sub = rx.getSubchannel(sc);
                    if (sub.subChId != -1) {
                        si.bitrate_kbps = sub.bitrate();
                        break;
                    }
                }

                result.services.push_back(si);
            }

            cerr << "found: " << result.ensemble_label
                 << " (" << result.services.size() << " services, SNR "
                 << fixed << setprecision(1) << result.snr << " dB)" << endl;

            results.push_back(result);
            rx.stop();
        }

        channelName = channels.getNextChannel();
    }
}

/* ---------- JSON report ---------- */

static string hex_str(uint32_t value, int width = 4)
{
    ostringstream oss;
    oss << "0x" << hex << uppercase << setw(width) << setfill('0') << value;
    return oss.str();
}

void ChannelScanner::printJsonReport(std::ostream& out) const
{
    /* ISO-8601 UTC timestamp */
    auto now = system_clock::now();
    time_t t = system_clock::to_time_t(now);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));

    json j;
    j["scan"]["timestamp"]       = tsbuf;
    j["scan"]["channels_scanned"] = NUMBEROFCHANNELS;
    j["scan"]["ensembles_found"]  = static_cast<int>(results.size());

    json arr = json::array();
    for (const auto& r : results) {
        json entry;
        entry["channel"]           = r.channel;
        entry["frequency_hz"]      = r.frequency_hz;
        entry["ensemble"]["id"]    = hex_str(r.ensemble_id);
        entry["ensemble"]["label"] = r.ensemble_label;
        entry["snr_db"]            = r.snr;

        json svc_arr = json::array();
        for (const auto& s : r.services) {
            json svc;
            svc["sid"]          = hex_str(s.sid);
            svc["label"]        = s.label;
            svc["bitrate_kbps"] = s.bitrate_kbps;
            svc_arr.push_back(svc);
        }
        entry["services"] = svc_arr;
        arr.push_back(entry);
    }
    j["results"] = arr;

    out << j.dump(2) << "\n";
}
