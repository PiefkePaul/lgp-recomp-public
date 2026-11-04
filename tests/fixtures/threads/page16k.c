// 16 KiB of read-only data on a 16 KiB boundary. It moves .data at least one
// 16 KiB page past the end of .text, so a host with 16 KiB pages (Apple
// silicon) can make the text executable without the data's page (native
// execution, docs/analysis/56 stage C: W^X forbids a page that is both).
// Kept against --gc-sections by -Wl,-u in CMakeLists.txt.
const unsigned char lgp_fixture_page16k[0x4000] __attribute__((used, aligned(0x4000))) = {1};
