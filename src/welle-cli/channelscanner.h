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

#pragma once

#include <condition_variable>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "backend/radio-controller.h"
#include "backend/radio-receiver-options.h"

/*
 * ChannelScanner scans all Band III and Band L DAB channels in sequence.
 * For each channel it checks for signal presence, and if a DAB ensemble is
 * found it records the ensemble name and service list.
 * The result can be printed as a JSON report via printJsonReport().
 *
 * Usage:
 *   ChannelScanner scanner;
 *   scanner.run(inputDevice, rro, 10 /timeout per channel in seconds/);
 *   scanner.printJsonReport(std::cout);
 */
class ChannelScanner : public RadioControllerInterface {
public:
    struct ServiceInfo {
        uint32_t    sid;
        std::string label;
        int         bitrate_kbps;
    };

    struct ScanResult {
        std::string              channel;
        uint32_t                 frequency_hz;
        std::string              ensemble_label;
        uint16_t                 ensemble_id;
        float                    snr;
        std::vector<ServiceInfo> services;
    };

    /* Scan all channels. input must already be initialised and gain set.
     * timeout_per_channel_sec controls how long to wait for sync on a
     * channel where a signal was detected (default 10 s). */
    void run(InputInterface& input,
             const RadioReceiverOptions& rro,
             int timeout_per_channel_sec = 10);

    void printJsonReport(std::ostream& out) const;

    /* RadioControllerInterface callbacks */
    void onSNR(float snr) override;
    void onFrequencyCorrectorChange(int, int) override {}
    void onSyncChange(char isSync) override;
    void onSignalPresence(bool isSignal) override;
    void onServiceDetected(uint32_t sId) override;
    void onNewEnsemble(uint16_t eId) override;
    void onSetEnsembleLabel(DabLabel& label) override;
    void onDateTimeUpdate(const dab_date_time_t&) override {}
    void onFIBDecodeSuccess(bool, const uint8_t*) override {}
    void onNewImpulseResponse(std::vector<float>&&) override {}
    void onConstellationPoints(std::vector<DSPCOMPLEX>&&) override {}
    void onNewNullSymbol(std::vector<DSPCOMPLEX>&&) override {}
    void onTIIMeasurement(tii_measurement_t&&) override {}
    void onMessage(message_level_t, const std::string&,
                   const std::string& = std::string()) override {}

private:
    /* Shared state updated from RadioReceiver callbacks */
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    signal_present   = false;
    bool                    synced           = false;
    float                   current_snr      = 0.0f;
    uint16_t                current_eid      = 0;
    std::string             current_label;
    std::set<uint32_t>      detected_sids;

    std::vector<ScanResult> results;
};
