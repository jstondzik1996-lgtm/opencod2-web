"""Regresje infrastruktury; syntetyczne dane NIE dowodzą działania gry."""
import argparse
import json
import math
import shutil
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

from browser_game_report import Report, frame_metrics, sync_metrics, log_findings
from browser_game_diagnostics import Diagnostics, FRAME_PROBE
from run_browser_game_test import preflight, asset_path, run, udp_status, stop_owned, monitor_worker, server_command

def samples():
    return [{"t_ms": i*250, "clients": [
        {"state":8, "client":c, "psClient":c, "serverTime":1100+i*250, "snapshotTime":1000+i*250}
        for c in range(2)]} for i in range(25)]

class MetricsTests(unittest.TestCase):
    def test_performance_good_low_fps_and_long_stall(self):
        self.assertTrue(frame_metrics([1000/60]*600)["pass"])
        self.assertFalse(frame_metrics([100]*100)["pass"])
        self.assertFalse(frame_metrics([16]*599+[1500])["pass"])
    def test_performance_rejects_empty_nan_infinite_and_negative(self):
        for values in ([], [16]*29, [math.nan]*40, [math.inf]*40, [-1]*40, [0]*40):
            self.assertFalse(frame_metrics(values)["pass"])
    def test_p95_threshold_independent_of_average(self):
        values = [10]*90 + [180]*10
        self.assertFalse(frame_metrics(values)["pass"])
        self.assertGreater(frame_metrics(values)["fps"], 20)
    def test_snapshots_advance(self):
        self.assertTrue(sync_metrics(samples())["pass"])
    def test_frozen_one_client_is_detected(self):
        rows = samples()
        for r in rows:
            r["clients"][1]["snapshotTime"] = 1000
        self.assertFalse(sync_metrics(rows)["pass"])
    def test_freeze_then_recovery_still_fails(self):
        rows = samples()
        for r in rows[1:20]:
            r["clients"][0]["snapshotTime"] = 1000
        self.assertFalse(sync_metrics(rows)["pass"])
    def test_identity_disconnect_and_backward_clock(self):
        for field, value in (("psClient",9), ("state",7), ("snapshotTime",-1),
                             ("snapshotTime",math.nan), ("serverTime",999999)):
            rows = samples()
            rows[10]["clients"][0][field] = value
            self.assertFalse(sync_metrics(rows)["pass"])
    def test_short_observation_fails(self):
        self.assertFalse(sync_metrics(samples()[:3])["pass"])
    def test_missing_model_material_and_fatal(self):
        result = log_findings([
            "WARNING: Could not find material 'bad'",
            "ERROR: model xmodel/bad not found",
            "[cg-player] missing DObj entity=1",
            "RuntimeError: memory access out of bounds",
            "WARNING: ordinary nonfatal information",
        ])
        self.assertEqual(len(result["assets"]), 3)
        self.assertEqual(len(result["fatal"]), 1)
    def test_asset_findings_redact_ticket(self):
        result = log_findings(["failed to load model ?ticket=private"])
        self.assertNotIn("private", json.dumps(result))
    def test_page_crash_is_not_silently_ignored(self):
        d = Diagnostics()
        d.event("crash0", "Awaria klienta")
        self.assertTrue(d.findings()["page_errors"])

class FrameProbeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"),"Brak Node.js do sprawdzenia skryptu pomiarowego")
    def test_raf_without_draw_is_not_counted(self):
        js = """
const vm = require('vm');
const assert = require('assert');
let callback;
class GL { drawArrays() {} }
const window = {WebGLRenderingContext:GL};
const sandbox = {window, document:{addEventListener(){}}, console,
  requestAnimationFrame(fn){callback=fn;}};
vm.runInNewContext(PROBE, sandbox);
const p=window.__cod2Probe;
p.enabled=true;
for(let t=0;t<20;t++) callback(t*16);
assert.equal(p.intervals.length,0);
const gl=new GL();
for(let t=20;t<80;t++) {
  if(t%2===0) gl.drawArrays();
  callback(t*16);
}
assert(p.intervals.length>20);
assert(p.intervals.every(t => t===32));
""".replace("PROBE",json.dumps(FRAME_PROBE))
        result=subprocess.run(["node","-e",js],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)

class ReportTests(unittest.TestCase):
    def test_unexecuted_checks_never_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Report(directory)
            report.check("preflight",True)
            self.assertEqual(report.write(),1)
            data = json.loads((Path(directory)/"raport.json").read_text(encoding="utf-8"))
            self.assertEqual(data["status"],"FAIL")
            self.assertEqual(data["checks"]["target_died"]["status"],"NIE_WYKONANO")
            self.assertIn("Raport automatycznego testu", (Path(directory)/"raport.md").read_text(encoding="utf-8"))
    def test_all_checks_and_one_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Report(directory)
            for key in report.data["checks"]:
                report.check(key,True)
            self.assertEqual(report.write(),0)
            report.check("damage_to_other_player",False)
            self.assertEqual(report.write(),1)

class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root/"main").mkdir()
        (self.root/"cod2.html").write_text("<canvas></canvas>")
        (self.root/"cod2.js").write_text("fixture")
        (self.root/"cod2.wasm").write_bytes(b"\0asm\x01\0\0\0")
        self.exe = self.root/"server.exe"
        self.exe.write_bytes(b"MZfixture")
        with zipfile.ZipFile(self.root/"main/test.iwd","w") as archive:
            archive.writestr("xmodel/test","fixture")
            archive.writestr("materials/test","fixture")
        self.manifest = {"files":[{"path":"main/test.iwd",
            "size":(self.root/"main/test.iwd").stat().st_size}]}
        self.save_manifest()
    def save_manifest(self):
        (self.root/"assets.json").write_text(json.dumps(self.manifest))
    def test_server_fixture_uses_direct_arguments_not_unresolved_cfg(self):
        command=server_command(self.exe,self.root,self.root/"own home",28963)
        self.assertEqual(command[-2:],["+devmap","mp_toujane"])
        self.assertNotIn("+exec",command)
        self.assertIn(str(self.root/"own home"),command)
        i=command.index("net_ip")
        self.assertEqual(command[i+1],"127.0.0.1")
    def test_valid_local_layout(self):
        result = preflight(self.root,self.exe,self.root)
        self.assertEqual(result["models"],1)
        self.assertEqual(result["materials"],1)
        self.assertFalse(result["crc_checked"])
    def test_bad_wasm_is_not_html_success(self):
        (self.root/"cod2.wasm").write_bytes(b"<html>404</html>")
        with self.assertRaises(ValueError):
            preflight(self.root,self.exe,self.root)
    def test_missing_wrong_size_duplicate_and_corrupt_iwd(self):
        self.manifest["files"][0]["size"] += 1
        self.save_manifest()
        with self.assertRaises(ValueError):
            preflight(self.root,self.exe,self.root)
        self.manifest["files"][0]["size"] -= 1
        self.manifest["files"] *= 2
        self.save_manifest()
        with self.assertRaises(ValueError):
            preflight(self.root,self.exe,self.root)
        (self.root/"main/test.iwd").unlink()
        with self.assertRaises(ValueError):
            preflight(self.root,self.exe,self.root)
    def test_corrupt_archive_and_missing_model_index(self):
        path=self.root/"main/test.iwd"
        path.write_bytes(b"not a zip")
        self.manifest["files"][0]["size"]=path.stat().st_size
        self.save_manifest()
        with self.assertRaises(zipfile.BadZipFile):
            preflight(self.root,self.exe,self.root)
        with zipfile.ZipFile(path,"w") as archive:
            archive.writestr("materials/test","fixture")
        self.manifest["files"][0]["size"]=path.stat().st_size
        self.save_manifest()
        with self.assertRaises(ValueError):
            preflight(self.root,self.exe,self.root)
    def test_manifest_traversal_and_external_urls_rejected(self):
        for name in ("../secret","/absolute","C:/secret","main/../../secret",
                     "https://example.com/a","main\\secret","file?token=bad",""):
            with self.assertRaises(ValueError):
                asset_path(self.root,name)
    def args(self):
        return argparse.Namespace(web_root=str(self.root),server_exe=str(self.exe),
            game_root=str(self.root),output=str(self.root/"reports"),source_revision="test",
            preflight_only=False,timeout=10,min_fps=20,max_p95_ms=150,browser=None,
            observe_seconds=12,max_stall_ms=3000,max_gap_ms=1500)
    @patch("run_browser_game_test.importlib.util.find_spec",return_value=object())
    def test_socket_permission_error_still_writes_report(self,_):
        with patch("run_browser_game_test.choose_ports",side_effect=PermissionError("blokada localhost")):
            self.assertEqual(run(self.args()),1)
        data = json.loads(next((self.root/"reports").glob("*/raport.json")).read_text(encoding="utf-8"))
        self.assertEqual(data["checks"]["services"]["status"],"FAIL")
        self.assertEqual(data["checks"]["target_died"]["status"],"NIE_WYKONANO")
    @patch("run_browser_game_test.importlib.util.find_spec",return_value=object())
    def test_preflight_only_never_claims_game_pass(self,_):
        args = self.args()
        args.preflight_only=True
        with patch("run_browser_game_test.start_owned") as start:
            self.assertEqual(run(args),1)
            start.assert_not_called()
    @patch("run_browser_game_test.importlib.util.find_spec",return_value=object())
    def test_start_failure_cleans_only_processes_started_here(self,_):
        proc, handle = Mock(), Mock()
        with patch("run_browser_game_test.choose_ports",return_value=[40001,40002,40003,40004]), \
             patch("run_browser_game_test.start_owned",side_effect=[(proc,handle),OSError("start failed")]), \
             patch("run_browser_game_test.stop_owned") as stop:
            self.assertEqual(run(self.args()),1)
            stop.assert_called_once_with(proc,tree=False)
            handle.close.assert_called_once()
    def test_do_not_kill_an_already_exited_pid(self):
        proc=Mock()
        proc.poll.return_value=0
        with patch("run_browser_game_test.subprocess.run") as kill:
            stop_owned(proc)
            kill.assert_not_called()
    def test_udp_wrong_payload_and_peer_rejected(self):
        for payload,peer in ((b"garbage",("127.0.0.1",28961)),
                             (b"\xff\xff\xff\xffstatusResponse\n",("127.0.0.1",9))):
            sock=Mock()
            sock.recvfrom.return_value=(payload,peer)
            context=Mock()
            context.__enter__=Mock(return_value=sock)
            context.__exit__=Mock(return_value=False)
            with patch("run_browser_game_test.socket.socket",return_value=context):
                with self.assertRaises(ValueError):
                    udp_status(28961)

class WatchdogTests(unittest.TestCase):
    def test_hung_client_times_out_without_waiting_for_page_evaluate(self):
        proc=Mock()
        proc.poll.return_value=None
        args=argparse.Namespace(timeout=1,max_stall_ms=3000)
        with patch("run_browser_game_test.time.monotonic",side_effect=[0,0,2]), \
             patch("run_browser_game_test.time.sleep"):
            with self.assertRaisesRegex(TimeoutError,"klient"):
                monitor_worker([(proc,None)],(proc,None),123,args,[])
    def test_nonresponding_dedicated_server_times_out(self):
        proc=Mock()
        proc.poll.return_value=None
        args=argparse.Namespace(timeout=60,max_stall_ms=1000)
        with patch("run_browser_game_test.time.monotonic",side_effect=[0,0,0,2,2]), \
             patch("run_browser_game_test.udp_status",side_effect=TimeoutError()), \
             patch("run_browser_game_test.time.sleep"):
            with self.assertRaisesRegex(TimeoutError,"dedykowany"):
                monitor_worker([(proc,None)],(proc,None),123,args,[])
    def test_crashed_service_fails_immediately(self):
        service,worker=Mock(),Mock()
        service.poll.return_value=1
        worker.poll.return_value=None
        with self.assertRaisesRegex(RuntimeError,"zakończył"):
            monitor_worker([(service,None),(worker,None)],(worker,None),123,
                           argparse.Namespace(timeout=60,max_stall_ms=3000),[])

if __name__ == "__main__":
    unittest.main()
