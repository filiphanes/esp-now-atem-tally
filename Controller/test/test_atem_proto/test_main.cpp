#include <unity.h>
#include <ATEMmini.h>
#include <cstring>

using namespace atemmini;

void setUp() {}
void tearDown() {}

// ---- header accessors ----

void test_header_accessors() {
  // flags=HELLO(0x2), len=0x234, session 0xABCD, ack id 0, packet id 0x1234
  uint8_t pkt[] = {0x12, 0x34, 0xAB, 0xCD, 0x00, 0x00,
                   0x11, 0x22, 0x00, 0x00, 0x12, 0x34};
  TEST_ASSERT_EQUAL_UINT8(F_HELLO, headerFlags(pkt));
  TEST_ASSERT_EQUAL_HEX16(0x234, headerLength(pkt));
  TEST_ASSERT_EQUAL_HEX16(0xABCD, headerSessionId(pkt));
  TEST_ASSERT_EQUAL_HEX16(0x1122, headerRequestNextId(pkt));
  TEST_ASSERT_EQUAL_HEX16(0x1234, headerPacketId(pkt));
}

// ---- builders ----

void test_build_hello_layout() {
  uint8_t buf[32];
  memset(buf, 0xEE, sizeof(buf));   // builder must fully overwrite
  size_t n = buildHello(buf, sizeof(buf), PROVISIONAL_SESSION_ID);
  TEST_ASSERT_EQUAL_UINT32(20, n);
  TEST_ASSERT_EQUAL_UINT8((F_HELLO << 3) | ((20 >> 8) & 7), buf[0]);
  TEST_ASSERT_EQUAL_UINT8(20 & 0xFF, buf[1]);
  TEST_ASSERT_EQUAL_HEX16(PROVISIONAL_SESSION_ID, (buf[2] << 8) | buf[3]);
  TEST_ASSERT_EQUAL_UINT8(0x3A, buf[9]);
  TEST_ASSERT_EQUAL_UINT8(0x01, buf[12]);
}

void test_build_hello_too_small_returns_0() {
  uint8_t buf[19];
  TEST_ASSERT_EQUAL_UINT32(0, buildHello(buf, sizeof(buf), PROVISIONAL_SESSION_ID));
}

void test_build_hello_ack() {
  uint8_t buf[16];
  memset(buf, 0xEE, sizeof(buf));
  size_t n = buildHelloAck(buf, sizeof(buf), 0x1234);
  TEST_ASSERT_EQUAL_UINT32(12, n);
  TEST_ASSERT_EQUAL_UINT8(F_ACK << 3, buf[0]);   // len 12 => length MSB bits 0
  TEST_ASSERT_EQUAL_UINT8(12, buf[1]);
  TEST_ASSERT_EQUAL_HEX16(0x1234, (buf[2] << 8) | buf[3]);
  TEST_ASSERT_EQUAL_UINT8(0x03, buf[9]);
  // no local packet id consumed
  TEST_ASSERT_EQUAL_HEX16(0, headerPacketId(buf));
}

void test_build_ack_sets_remote_id_no_local_id() {
  uint8_t buf[16];
  memset(buf, 0, sizeof(buf));
  size_t n = buildAck(buf, sizeof(buf), 0x53AB, 0x0777);
  TEST_ASSERT_EQUAL_UINT32(12, n);
  TEST_ASSERT_EQUAL_UINT8(F_ACK << 3, buf[0]);
  TEST_ASSERT_EQUAL_HEX16(0x53AB, headerSessionId(buf));
  TEST_ASSERT_EQUAL_HEX16(0x0777, headerAckId(buf));
  TEST_ASSERT_EQUAL_HEX16(0, headerPacketId(buf));
}

void test_build_empty_resend_carries_wanted_id() {
  uint8_t buf[16];
  size_t n = buildEmptyResend(buf, sizeof(buf), 0x53AB, 0x0021);
  TEST_ASSERT_EQUAL_UINT32(12, n);
  TEST_ASSERT_EQUAL_UINT8(F_ACK_REQUEST << 3, buf[0]);
  TEST_ASSERT_EQUAL_HEX16(0x21, headerPacketId(buf));
}

void test_build_resend_request() {
  uint8_t buf[16];
  size_t n = buildResendRequest(buf, sizeof(buf), 0x53AB, 5);
  TEST_ASSERT_EQUAL_UINT32(12, n);
  TEST_ASSERT_EQUAL_UINT8(F_REQUEST_NEXT << 3, buf[0]);
  TEST_ASSERT_EQUAL_HEX16(5, headerRequestNextId(buf));
  TEST_ASSERT_EQUAL_UINT8(0x01, buf[8]);
}

void test_build_command_cpgi() {
  uint8_t buf[64];
  memset(buf, 0xEE, sizeof(buf));
  uint16_t localId = 41;
  uint8_t payload[4] = {0, 0, 0, 7};   // M/E 0, source 7
  size_t n = buildCommand(buf, sizeof(buf), 0x53AB, &localId, "CPgI", payload, 4);
  TEST_ASSERT_EQUAL_UINT32(24, n);     // 12 header + 8 seg header + 4 payload
  TEST_ASSERT_EQUAL_UINT8((F_ACK_REQUEST << 3) | ((24 >> 8) & 7), buf[0]);
  TEST_ASSERT_EQUAL_UINT8(24 & 0xFF, buf[1]);
  TEST_ASSERT_EQUAL_HEX16(0x53AB, headerSessionId(buf));
  TEST_ASSERT_EQUAL_UINT32(42, localId);          // incremented exactly once
  TEST_ASSERT_EQUAL_HEX16(42, headerPacketId(buf));
  // segment: length field 12, cmd string at +4, payload at +8
  TEST_ASSERT_EQUAL_HEX16(12, (buf[12] << 8) | buf[13]);
  TEST_ASSERT_EQUAL_UINT8('C', buf[16]);
  TEST_ASSERT_EQUAL_UINT8('P', buf[17]);
  TEST_ASSERT_EQUAL_UINT8('g', buf[18]);
  TEST_ASSERT_EQUAL_UINT8('I', buf[19]);
  TEST_ASSERT_EQUAL_UINT8(0, buf[20]);
  TEST_ASSERT_EQUAL_UINT8(0, buf[21]);
  TEST_ASSERT_EQUAL_UINT8(0, buf[22]);
  TEST_ASSERT_EQUAL_UINT8(7, buf[23]);
}

void test_build_command_too_small_returns_0_and_keeps_counter() {
  uint8_t buf[23];   // one byte short
  uint16_t localId = 10;
  uint8_t payload[4] = {0, 0, 0, 1};
  TEST_ASSERT_EQUAL_UINT32(0, buildCommand(buf, sizeof(buf), 0x53AB, &localId, "CPgI", payload, 4));
  TEST_ASSERT_EQUAL_UINT32(10, localId);          // not consumed on failure
}

// ---- tally parsing ----

void test_parse_tally_bits() {
  // count=4 sources: in1=pgm, in2=pvw, in3=both, in4=off
  uint8_t pl[] = {0, 4, 0x01, 0x02, 0x03, 0x00};
  uint64_t pgm = 0, pvw = 0;
  parseTally(pl, sizeof(pl), &pgm, &pvw);
  TEST_ASSERT_EQUAL_UINT64(0x1 | 0x4, pgm);
  TEST_ASSERT_EQUAL_UINT64(0x2 | 0x4, pvw);
}

void test_parse_tally_clamps_count_to_64() {
  uint8_t pl[66] = {0};
  pl[0] = 0; pl[1] = 100;                 // claims 100 sources
  memset(pl + 2, 0xFF, 64);               // all 64 present in pgm+pvw
  uint64_t pgm = 0, pvw = 0;
  parseTally(pl, sizeof(pl), &pgm, &pvw);
  TEST_ASSERT_EQUAL_UINT64(0xFFFFFFFFFFFFFFFFull, pgm);
  TEST_ASSERT_EQUAL_UINT64(0xFFFFFFFFFFFFFFFFull, pvw);
}

void test_parse_tally_clamps_to_available_bytes() {
  // count says 8 but only 2 flag bytes present
  uint8_t pl[] = {0, 8, 0x01, 0x02};
  uint64_t pgm = 0xFF, pvw = 0xFF;
  parseTally(pl, sizeof(pl), &pgm, &pvw);
  TEST_ASSERT_EQUAL_UINT64(0x1, pgm);
  TEST_ASSERT_EQUAL_UINT64(0x2, pvw);
}

void test_parse_tally_short_payload_is_safe() {
  uint64_t pgm = 0xAA, pvw = 0xBB;
  uint8_t pl[] = {0};
  parseTally(pl, sizeof(pl), &pgm, &pvw);
  TEST_ASSERT_EQUAL_UINT64(0, pgm);
  TEST_ASSERT_EQUAL_UINT64(0, pvw);
}

// ---- segment walking ----

static uint64_t cb_pgm, cb_pvw;
static int cb_calls;
static char cb_last_cmd[5];

static void tallyCollector(void *user, const char cmd5[5],
                           const uint8_t *payload, uint16_t len) {
  cb_calls++;
  strncpy(cb_last_cmd, cmd5, 4);
  cb_last_cmd[4] = '\0';
  if (strcmp(cmd5, "TlIn") == 0 && user == nullptr) {
    parseTally(payload, len, &cb_pgm, &cb_pvw);
  }
}

// Helper: append one segment to a buffer, return new length.
static size_t pushSeg(uint8_t *buf, size_t off, const char *cmd,
                      const uint8_t *payload, size_t plen) {
  size_t segLen = 8 + plen;
  buf[off] = segLen >> 8;
  buf[off + 1] = segLen & 0xFF;
  memcpy(&buf[off + 4], cmd, 4);
  if (plen) memcpy(&buf[off + 8], payload, plen);
  return off + segLen;
}

void test_for_each_segment_dispatches_tlIn_and_skips_unknown() {
  uint8_t pkt[128] = {0};
  // header: len filled at the end, session etc. irrelevant here
  size_t off = 12;
  uint8_t tl[] = {0, 3, 0x01, 0x02, 0x03};            // 3 sources
  off = pushSeg(pkt, off, "TlIn", tl, sizeof(tl));
  uint8_t junk[] = {1, 2, 3, 4};
  off = pushSeg(pkt, off, "FASP", junk, sizeof(junk)); // unknown -> skipped by consumer
  pkt[0] = (off >> 8) & 0x07;
  pkt[1] = off & 0xFF;

  cb_calls = 0; cb_pgm = cb_pvw = 0; cb_last_cmd[0] = '\0';
  forEachSegment(pkt, off, tallyCollector, nullptr);

  TEST_ASSERT_EQUAL_INT(2, cb_calls);                  // both segments visited
  TEST_ASSERT_EQUAL_STRING("FASP", cb_last_cmd);       // last one wins for the name
  TEST_ASSERT_EQUAL_UINT64(0x1 | 0x4, cb_pgm);
  TEST_ASSERT_EQUAL_UINT64(0x2 | 0x4, cb_pvw);
}

void test_for_each_segment_stops_on_malformed_length() {
  uint8_t pkt[64] = {0};
  size_t off = 12;
  uint8_t tl[] = {0, 1, 0x01};
  off = pushSeg(pkt, off, "TlIn", tl, sizeof(tl));
  // malformed: claims 200 bytes but only a few remain
  pkt[off] = 200 >> 8;
  pkt[off + 1] = 200 & 0xFF;
  memcpy(&pkt[off + 4], "BAD!", 4);
  off += 8;
  pkt[0] = (off >> 8) & 0x07;
  pkt[1] = off & 0xFF;

  cb_calls = 0; cb_pgm = cb_pvw = 0;
  forEachSegment(pkt, off, tallyCollector, nullptr);

  TEST_ASSERT_EQUAL_INT(1, cb_calls);                  // walked only the good one
  TEST_ASSERT_EQUAL_UINT64(0x1, cb_pgm);
}

void test_for_each_segment_empty_datagram() {
  uint8_t pkt[12] = {0};
  cb_calls = 0;
  forEachSegment(pkt, 12, tallyCollector, nullptr);
  TEST_ASSERT_EQUAL_INT(0, cb_calls);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_header_accessors);
  RUN_TEST(test_build_hello_layout);
  RUN_TEST(test_build_hello_too_small_returns_0);
  RUN_TEST(test_build_hello_ack);
  RUN_TEST(test_build_ack_sets_remote_id_no_local_id);
  RUN_TEST(test_build_empty_resend_carries_wanted_id);
  RUN_TEST(test_build_resend_request);
  RUN_TEST(test_build_command_cpgi);
  RUN_TEST(test_build_command_too_small_returns_0_and_keeps_counter);
  RUN_TEST(test_parse_tally_bits);
  RUN_TEST(test_parse_tally_clamps_count_to_64);
  RUN_TEST(test_parse_tally_clamps_to_available_bytes);
  RUN_TEST(test_parse_tally_short_payload_is_safe);
  RUN_TEST(test_for_each_segment_dispatches_tlIn_and_skips_unknown);
  RUN_TEST(test_for_each_segment_stops_on_malformed_length);
  RUN_TEST(test_for_each_segment_empty_datagram);
  return UNITY_END();
}
