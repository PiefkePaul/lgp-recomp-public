"""Unit tests for tools/analyze/ryujinx_log_stats.py on synthetic log lines (no game data).

Run: python -m unittest discover -s tests/tools -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "analyze"))
import ryujinx_log_stats as rls  # noqa: E402

LOG = """\
00:00:00.209 |I| Configuration LogValueChange: EnableDebug set to: True
00:00:01.722 |T| HLE.GuestThread.46 KernelSvc : GetInfo(id: TotalMemorySize, handle: 0xFFFF8001, subId: 0x0000000000000000)
00:00:01.722 |T| HLE.GuestThread.46 KernelSvc : GetInfo(value: 0x00000000C0000000) = Success
00:00:01.900 |T| <MainThread> KernelSvc : GetInfo(id: UsedMemorySize, handle: 0xFFFF8001, subId: 0x0000000000000000)
00:00:01.900 |T| <MainThread> KernelSvc : GetInfo(value: 0x0000000004000000) = Success
00:00:01.927 |T| <MainThread> KernelSvc : SetHeapSize(size: 0x00000000B0000000)
00:00:06.621 |T| <MainThread> KernelSvc : SetHeapSize(address: 0x000000208C600000) = Success
00:00:07.434 |T| <MainThread> KernelSvc : CreateThread(entrypoint: 0x0000000000001000, argsPtr: 0x0, stackTop: 0x0, priority: 0x0000002C, cpuCore: 0x00000001)
00:00:07.434 |T| <MainThread> KernelSvc : CreateThread(handle: 0x00068008) = Success
00:00:07.440 |T| <MainThread> KernelSvc : CreateThread(entrypoint: 0x0000000000001000, argsPtr: 0x0, stackTop: 0x0, priority: 0x0000002B, cpuCore: 0xFFFFFFFE)
00:00:07.440 |T| <MainThread> KernelSvc : CreateThread(handle: 0x00070009) = Success
00:00:07.500 |T| <Worker Thread> KernelSvc : GetCurrentProcessorNumber()
00:00:07.500 |T| <Worker Thread> KernelSvc : GetCurrentProcessorNumber() = 0x00000001
00:00:07.501 |T| <Worker Thread> KernelSvc : WaitSynchronization(handlesPtr: 0x1, handlesCount: 0x1, timeout: 0x0)
00:00:07.502 |T| <Worker Thread> KernelSvc : WaitSynchronization(handleIndex: 0x0) = TimedOut
00:00:07.503 |T| <Worker Thread> KernelSvc : SleepThread(timeout: 0x0000000000989680)
00:00:07.504 |T| <Worker Thread> KernelSvc : SleepThread()
00:00:07.505 |T| <Worker Thread> KernelSvc : ExitThread()
00:00:07.506 |T| <Worker Thread> KernelSvc : ExitThread()
00:00:07.577 |T| HLE.OsThread.2 KernelIpc CallCmifMethod: IFileSystemProxy: SetCurrentProcess
00:00:07.578 |T| HLE.OsThread.2 KernelIpc CallCmifMethod: IFileSystemProxy: OpenDataStorageByCurrentProcess
00:00:07.579 |T| HLE.OsThread.2 KernelIpc CallCmifMethod: IStorage: Read
00:00:07.580 |T| HLE.OsThread.2 KernelIpc CallCmifMethod: IStorage: Read
00:00:01.786 |T| HLE.OsThread.18 KernelIpc ProcessMessageImpl: LogService.OpenLogger called
00:00:07.771 |S| HLE.OsThread.4 ServiceHid SetSupportedNpadStyleSet: Stubbed. NpadStyleSet: FullKey
00:00:07.772 |W| HLE.OsThread.4 Hid Remap: something
00:00:03.150 |D| HLE.OsThread.4 ServiceNv Create: Created map 4 with size 0x00001000!
00:00:03.159 |D| HLE.OsThread.4 ServiceNv Create: Created map 12 with size 0x0a400000!
00:00:06.454 |D| HLE.OsThread.4 ServiceNv DecrementMapRefCount: Deleted map 4!
00:00:06.455 |D| HLE.OsThread.4 ServiceNv Create: Created map 16 with size 0x00002000!
00:01:59.978 |I| HLE.OsThread.47 ServiceAm .ctor: Applet 'SoftwareKeyboard' created.
not a log line
"""


class RyujinxLogStatsTest(unittest.TestCase):
    def setUp(self):
        self.d = rls.analyze(LOG.splitlines(keepends=True)).to_dict()

    def test_svc_counts(self):
        svc = self.d["svc_calls"]
        self.assertEqual(svc["GetInfo"], 2)
        self.assertEqual(svc["CreateThread"], 2)
        self.assertEqual(svc["SetHeapSize"], 1)
        self.assertEqual(svc["GetCurrentProcessorNumber"], 1)
        self.assertEqual(svc["SleepThread"], 1)
        self.assertEqual(svc["ExitThread"], 1)

    def test_return_values_are_not_errors(self):
        self.assertEqual(self.d["svc_non_success"], {"WaitSynchronization=TimedOut": 1})

    def test_ipc_commands(self):
        ipc = self.d["ipc_commands"]
        self.assertEqual(ipc["IStorage.Read"], 2)
        self.assertEqual(ipc["IFileSystemProxy.SetCurrentProcess"], 1)
        self.assertEqual(ipc["LogService.OpenLogger"], 1)
        self.assertEqual(self.d["ipc_interfaces"]["IFileSystemProxy"], 2)

    def test_memory(self):
        mem = self.d["memory"]
        self.assertEqual(mem["heap_size_max"], 0xB0000000)
        self.assertEqual(mem["total_memory_size"], 0xC0000000)
        self.assertEqual(mem["used_memory_size_max"], 0x04000000)

    def test_nvmap_and_applets(self):
        mem = self.d["memory"]
        self.assertEqual(mem["nvmap_created"], 3)
        self.assertEqual(mem["nvmap_peak_bytes"], 0x0A402000)
        self.assertEqual(mem["nvmap_live_bytes_at_end"], 0x0A402000)
        self.assertEqual(self.d["applets"], {"SoftwareKeyboard": 1})

    def test_threads(self):
        self.assertEqual(self.d["created_threads"], 2)
        self.assertEqual(self.d["created_thread_priorities"], {"43": 1, "44": 1})
        self.assertEqual(self.d["created_thread_cores"], {"-2": 1, "1": 1})
        names = [t["name"] for t in self.d["threads"]]
        self.assertEqual(names, ["HLE.GuestThread.46", "<MainThread>", "<Worker Thread>"])

    def test_stubs_and_warnings(self):
        self.assertEqual(self.d["stubs"], {"ServiceHid.SetSupportedNpadStyleSet": 1})
        self.assertEqual(self.d["warnings"], {"Hid.Remap": 1})

    def test_markdown_renders(self):
        md = rls.to_markdown(self.d)
        self.assertIn("IStorage.Read", md)
        self.assertIn("3.00 GiB", md)


if __name__ == "__main__":
    unittest.main()
