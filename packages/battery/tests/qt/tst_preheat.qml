// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
import QtQuick 2.0
import QtTest 1.0
import "HomeScreen"

Item {
    id: scene
    width: 1920
    height: 720
    property string lastPost: ""
    property int postCount: 0

    BatteryPage {
        id: page
        width: scene.width
        height: 630
        // Never touch the daemon socket in a host test; record the endpoint
        // the real toggle would have called.
        function post(path) {
            scene.lastPost = path
            scene.postCount++
        }
        function refresh() {}
    }

    function baseStatus() {
        return {
            state: "idle", precond_raw: 0, precond_confirmed: false,
            batt_min_c: 7, batt_valid: true, batt_raw: "", temp_source: "0x2FC b2",
            soc_pct: 78.5, soc_valid: true, ready: true, utility: false,
            dash: false, factory_nav: false, retries: 0, eta_s: 0, message: "",
            park: true, park_known: true, gear: 0, cable: false, cable_known: true,
            charging: false, door_state: 2, door_known: true, door_source: "can",
            door_warn: 0, door_opt: 0, door_busy: false, door_message: "",
            last_tx: "", ver: "test", send_ok: 0, send_fail: 0,
            batt_cond: 0, batt_cond_known: false, gear_raw: "", gear_b3: -1, gear_b4: -1,
            smack: "", smack_expected: "", smack_ok: true, smack_send: "", smack_monitor: "",
            batt_b2: 7, batt_b4: 0, batt_age_ms: 0, temp_byte: 2,
            micom_name: "", micom_path: "", dump_state: "idle", dump_message: "", dump_dir: "",
            home_seq: 0, home_source: "none", last_app_key: -1, home_monitor: false
        }
    }

    TestCase {
        name: "BatteryPreheat"
        when: windowShown

        function init() {
            page.serviceOk = true
            page.heaterState = "idle"
            page.preheatPending = false
            page.preheatRequested = false
            page.preheatRequestedAt = 0
            page.preheatRetries = 0
            page.blockReason = ""
            page.battValid = true
            page.battMin = 7
            scene.lastPost = ""
            scene.postCount = 0
        }

        // Regression: preheatPending was set before reading preheatOn, and
        // preheatOn includes preheatPending, so Start always sent /stop.
        function test_start_selects_start() {
            page.togglePreheat()
            compare(scene.lastPost, "/start")
            compare(page.preheatRequested, true)
        }

        function test_stop_selects_stop() {
            page.preheatRequested = true
            page.preheatPending = false
            page.togglePreheat()
            compare(scene.lastPost, "/stop")
            compare(page.preheatRequested, false)
        }

        // The 21 C cutoff must block starting only.
        function test_hot_battery_blocks_start() {
            page.battMin = 25
            page.togglePreheat()
            compare(scene.postCount, 0)
            compare(page.preheatPending, false)
        }

        function test_hot_battery_allows_stop() {
            page.battMin = 25
            page.heaterState = "active"
            page.togglePreheat()
            compare(scene.lastPost, "/stop")
        }

        function test_duplicate_click_suppressed() {
            page.preheatPending = true
            page.togglePreheat()
            compare(scene.postCount, 0)
        }

        // A timeout is an unconfirmed request, not success. Keep the switch
        // On so the user still has a Stop action: the heater may be running
        // without a visible 0x2AD status.
        function test_timeout_keeps_stop_affordance() {
            page.preheatRequested = true
            page.preheatRequestedAt = 0
            var d = baseStatus()
            d.state = "timeout"
            page.applyStatus(d)
            compare(page.preheatRequested, true)
            verify(page.heaterSubText().indexOf("tap to stop") >= 0)
            page.togglePreheat()
            compare(scene.lastPost, "/stop")
        }

        // A status poll that still says "timeout" must not cancel a request
        // whose POST is still in flight.
        function test_fresh_request_not_cleared_by_timeout() {
            page.preheatRequested = true
            page.preheatRequestedAt = Date.now()
            var d = baseStatus()
            d.state = "timeout"
            page.applyStatus(d)
            compare(page.preheatRequested, true)
        }

        // Once the daemon reports idle/stopped, the optimistic request ends.
        function test_idle_clears_request() {
            page.preheatRequested = true
            page.preheatRequestedAt = 0
            var d = baseStatus()
            d.state = "idle"
            page.applyStatus(d)
            compare(page.preheatRequested, false)
        }
    }
}
