#include <CommandBook.h>
#include <GatewayStorage.h>
#include <unity.h>

#include <string.h>

using namespace radiosensors::gateway_storage;
namespace command_book = radiosensors::command_book;
namespace protocol = radiosensors::protocol;
using command_book::CompleteStatus;
using command_book::QueueStatus;
using protocol::CommandStatus;
using protocol::CommandType;

namespace {

class MemorySlots final : public SlotStorage {
  public:
    bool exists(uint8_t slot) override {
        return slot < 2 && present_[slot];
    }

    bool read(uint8_t slot, uint8_t *output, size_t capacity, size_t &size) override {
        size = 0;
        if (slot >= 2 || !present_[slot] || sizes_[slot] > capacity)
            return false;
        memcpy(output, data_[slot], sizes_[slot]);
        size = sizes_[slot];
        return true;
    }

    bool write(uint8_t slot, const uint8_t *data, size_t size) override {
        if (slot >= 2 || data == nullptr || size > kCommandsSnapshotSize)
            return false;
        memcpy(data_[slot], data, size);
        sizes_[slot] = size;
        present_[slot] = true;
        return true;
    }

    void corrupt(uint8_t slot, size_t offset) {
        data_[slot][offset] ^= 0x80;
    }

  private:
    uint8_t data_[2][kCommandsSnapshotSize]{};
    size_t sizes_[2]{};
    bool present_[2]{};
};

uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xEDB88320UL : crc >> 1U;
    }
    return ~crc;
}

void resealCrc(uint8_t *snapshot) {
    protocol::writeUint32Le(snapshot + kCommandsSnapshotSize - 4,
                            crc32(snapshot, kCommandsSnapshotSize - 4));
}

struct Uid {
    uint8_t bytes[protocol::kDeviceUidSize];
};

Uid uidFor(uint8_t seed) {
    Uid uid{};
    for (size_t index = 0; index < sizeof(uid.bytes); ++index)
        uid.bytes[index] = static_cast<uint8_t>(seed + index);
    return uid;
}

const uint8_t kSetCount = static_cast<uint8_t>(CommandType::SetCount);

QueueStatus queueCount(CommandBook &book,
                       uint8_t nodeId,
                       uint32_t count,
                       uint16_t &id,
                       uint64_t now = 1000,
                       uint8_t uidSeed = 0) {
    const Uid uid = uidFor(uidSeed != 0 ? uidSeed : nodeId);
    uint8_t arguments[4];
    protocol::writeUint32Le(arguments, count);
    return command_book::queue(book, uid.bytes, nodeId, kSetCount, arguments, 4, now, id);
}

CompleteStatus completeUnsupported(CommandBook &book, uint8_t nodeId, uint16_t id) {
    const Uid uid = uidFor(nodeId);
    return command_book::complete(book,
                                  uid.bytes,
                                  nodeId,
                                  id,
                                  CommandStatus::Unsupported,
                                  nullptr,
                                  0,
                                  2000);
}

} // namespace

void setUp() {
}
void tearDown() {
}

void test_empty_command_store_loads_defaults() {
    MemorySlots slots;
    CommandStore store(slots);
    CommandBook book{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LoadStatus::Empty), static_cast<int>(store.load(book)));
    TEST_ASSERT_TRUE(commandBooksEqual(defaultCommandBook(), book));
    TEST_ASSERT_EQUAL_UINT16(1, book.nextCommandId);
    TEST_ASSERT_EQUAL_UINT8(0, book.count);
}

void test_command_book_known_layout() {
    CommandBook book = defaultCommandBook();
    book.nextCommandId = 0x1234;
    uint16_t id = 0;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(QueueStatus::Queued),
        static_cast<int>(queueCount(book, 7, 0x12345678UL, id, 0x0102030405060708ULL)));
    TEST_ASSERT_EQUAL_HEX16(0x1234, id);

    uint8_t snapshot[kCommandsSnapshotSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok),
                          static_cast<int>(encodeCommandBook(book, 5, snapshot, sizeof(snapshot))));
    TEST_ASSERT_EQUAL_MEMORY("RSCB", snapshot, 4);
    TEST_ASSERT_EQUAL_UINT16(kStorageVersion, protocol::readUint16Le(snapshot + 4));
    TEST_ASSERT_EQUAL_UINT32(5, protocol::readUint32Le(snapshot + 6));
    TEST_ASSERT_EQUAL_UINT16(kCommandsSnapshotSize, protocol::readUint16Le(snapshot + 10));
    TEST_ASSERT_EQUAL_UINT8(1, snapshot[12]);
    TEST_ASSERT_EQUAL_UINT8(0, snapshot[13]);
    TEST_ASSERT_EQUAL_HEX16(0x1235, protocol::readUint16Le(snapshot + 14));

    const uint8_t *record = snapshot + 16;
    const Uid uid = uidFor(7);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(uid.bytes, record, sizeof(uid.bytes));
    TEST_ASSERT_EQUAL_UINT8(7, record[10]);
    TEST_ASSERT_EQUAL_HEX16(0x1234, protocol::readUint16Le(record + 11));
    TEST_ASSERT_EQUAL_UINT8(1, record[13]);
    TEST_ASSERT_EQUAL_UINT8(4, record[14]);
    TEST_ASSERT_EQUAL_HEX32(0x12345678UL, protocol::readUint32Le(record + 15));
    TEST_ASSERT_EQUAL_HEX32(0, protocol::readUint32Le(record + 19));
    TEST_ASSERT_EQUAL_UINT8(1, record[23]);
    TEST_ASSERT_EQUAL_UINT8(0, record[24]);
    TEST_ASSERT_EQUAL_UINT8(0, record[25]);
    TEST_ASSERT_EQUAL_HEX32(0x05060708UL, protocol::readUint32Le(record + 34));
    TEST_ASSERT_EQUAL_HEX32(0x01020304UL, protocol::readUint32Le(record + 38));
    TEST_ASSERT_EQUAL_UINT32(0, protocol::readUint32Le(record + 42));

    CommandBook decoded{};
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::Ok),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));
    TEST_ASSERT_EQUAL_UINT32(5, generation);
    TEST_ASSERT_TRUE(commandBooksEqual(book, decoded));
}

void test_queue_rejects_invalid_requests() {
    CommandBook book = defaultCommandBook();
    uint16_t id = 0;
    const Uid uid = uidFor(7);
    const uint8_t shortCount[3] = {1, 2, 3};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::InvalidArguments),
                          static_cast<int>(command_book::queue(book,
                                                               uid.bytes,
                                                               7,
                                                               kSetCount,
                                                               shortCount,
                                                               sizeof(shortCount),
                                                               0,
                                                               id)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(QueueStatus::InvalidArguments),
        static_cast<int>(command_book::queue(book, uid.bytes, 7, 9, shortCount, 1, 0, id)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::InvalidNode),
                          static_cast<int>(queueCount(book, 0, 1, id)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::InvalidNode),
                          static_cast<int>(queueCount(book, 100, 1, id)));
    TEST_ASSERT_EQUAL_UINT8(0, book.count);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 7, 1, id)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Busy),
                          static_cast<int>(queueCount(book, 7, 5, id)));
    TEST_ASSERT_EQUAL_UINT8(1, book.count);
}

void test_read_info_queues_without_arguments_and_records_the_identity() {
    CommandBook book = defaultCommandBook();
    uint16_t id = 0;
    const Uid uid = uidFor(7);
    const uint8_t readInfo = static_cast<uint8_t>(protocol::CommandType::ReadInfo);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(QueueStatus::Queued),
        static_cast<int>(command_book::queue(book, uid.bytes, 7, readInfo, nullptr, 0, 0, id)));
    const uint8_t info[protocol::kReadInfoResultSize] = {6, 0, 1, 0, 2, 20};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CompleteStatus::InvalidResult),
        static_cast<int>(
            command_book::complete(book, uid.bytes, 7, id, CommandStatus::Applied, info, 5, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::Completed),
                          static_cast<int>(command_book::complete(book,
                                                                  uid.bytes,
                                                                  7,
                                                                  id,
                                                                  CommandStatus::Applied,
                                                                  info,
                                                                  sizeof(info),
                                                                  0)));
    TEST_ASSERT_EQUAL_UINT8(sizeof(info), command_book::find(book, 7)->resultSize);

    uint8_t snapshot[kCommandsSnapshotSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok),
                          static_cast<int>(encodeCommandBook(book, 1, snapshot, sizeof(snapshot))));
    CommandBook decoded{};
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::Ok),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));
    TEST_ASSERT_TRUE(commandBooksEqual(book, decoded));
}

void test_complete_records_result_and_rejects_mismatches() {
    CommandBook book = defaultCommandBook();
    uint16_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 7, 500, id)));
    const Uid uid = uidFor(7);
    const Uid otherUid = uidFor(90);
    uint8_t result[8];
    protocol::writeUint32Le(result, 12);
    protocol::writeUint32Le(result + 4, 500);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::NotPending),
                          static_cast<int>(command_book::complete(book,
                                                                  uid.bytes,
                                                                  7,
                                                                  id + 1,
                                                                  CommandStatus::Applied,
                                                                  result,
                                                                  8,
                                                                  0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::NotPending),
                          static_cast<int>(command_book::complete(book,
                                                                  otherUid.bytes,
                                                                  7,
                                                                  id,
                                                                  CommandStatus::Applied,
                                                                  result,
                                                                  8,
                                                                  0)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CompleteStatus::InvalidResult),
        static_cast<int>(
            command_book::complete(book, uid.bytes, 7, id, CommandStatus::Applied, result, 4, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::Retry),
                          static_cast<int>(command_book::complete(book,
                                                                  uid.bytes,
                                                                  7,
                                                                  id,
                                                                  CommandStatus::StorageFailure,
                                                                  nullptr,
                                                                  0,
                                                                  0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Pending),
                          static_cast<int>(command_book::find(book, 7)->state));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::Completed),
                          static_cast<int>(command_book::complete(book,
                                                                  uid.bytes,
                                                                  7,
                                                                  id,
                                                                  CommandStatus::Applied,
                                                                  result,
                                                                  8,
                                                                  3000)));
    const CommandRecord *record = command_book::find(book, 7);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Completed),
                          static_cast<int>(record->state));
    TEST_ASSERT_EQUAL_UINT8(8, record->resultSize);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(result, record->result, 8);
    TEST_ASSERT_EQUAL_UINT64(3000, record->completedAtUnixMs);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CompleteStatus::NotPending),
        static_cast<int>(
            command_book::complete(book, uid.bytes, 7, id, CommandStatus::Applied, result, 8, 0)));

    uint16_t nextId = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 7, 9, nextId)));
    TEST_ASSERT_EQUAL_UINT16(id + 1, nextId);
    TEST_ASSERT_EQUAL_UINT8(1, book.count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Pending),
                          static_cast<int>(command_book::find(book, 7)->state));
}

void test_record_of_a_previous_node_does_not_block_a_reused_id() {
    CommandBook book = defaultCommandBook();
    uint16_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 7, 1, id)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 7, 2, id, 0, 70)));
    TEST_ASSERT_EQUAL_UINT8(1, book.count);
    const Uid newUid = uidFor(70);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(newUid.bytes,
                                 command_book::find(book, 7)->deviceUid,
                                 sizeof(newUid.bytes));
}

void test_command_ids_skip_zero_on_wrap() {
    CommandBook book = defaultCommandBook();
    book.nextCommandId = UINT16_MAX;
    uint16_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 1, 1, id)));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, id);
    TEST_ASSERT_EQUAL_UINT16(1, book.nextCommandId);
    TEST_ASSERT_TRUE(command_book::cancel(book, 1));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 1, 1, id)));
    TEST_ASSERT_EQUAL_UINT16(1, id);
}

void test_full_book_evicts_the_oldest_result() {
    CommandBook book = defaultCommandBook();
    uint16_t ids[kMaxCommandRecords + 1]{};
    for (uint8_t node = 1; node <= kMaxCommandRecords; ++node) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                              static_cast<int>(queueCount(book, node, 1, ids[node])));
    }
    uint16_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::CapacityReached),
                          static_cast<int>(queueCount(book, 17, 1, id)));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::Completed),
                          static_cast<int>(completeUnsupported(book, 5, ids[5])));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::Completed),
                          static_cast<int>(completeUnsupported(book, 3, ids[3])));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 17, 1, id)));
    TEST_ASSERT_EQUAL_UINT8(kMaxCommandRecords, book.count);
    TEST_ASSERT_NULL(command_book::find(book, 3));
    TEST_ASSERT_NOT_NULL(command_book::find(book, 5));
    TEST_ASSERT_EQUAL_UINT8(17, book.records[kMaxCommandRecords - 1].nodeId);
}

void test_cancel_remove_and_clear() {
    CommandBook book = defaultCommandBook();
    uint16_t first = 0;
    uint16_t second = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 1, 1, first)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 2, 1, second)));
    TEST_ASSERT_FALSE(command_book::cancel(book, 3));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CompleteStatus::Completed),
                          static_cast<int>(completeUnsupported(book, 2, second)));
    TEST_ASSERT_FALSE(command_book::cancel(book, 2));
    TEST_ASSERT_TRUE(command_book::removeNode(book, 2));
    TEST_ASSERT_TRUE(command_book::cancel(book, 1));
    TEST_ASSERT_EQUAL_UINT8(0, book.count);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 4, 1, first)));
    const uint16_t next = book.nextCommandId;
    command_book::clear(book);
    TEST_ASSERT_EQUAL_UINT8(0, book.count);
    TEST_ASSERT_EQUAL_UINT16(next, book.nextCommandId);
}

void test_store_falls_back_to_the_previous_generation() {
    MemorySlots slots;
    CommandStore store(slots);
    CommandBook book = defaultCommandBook();
    uint16_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 1, 1, id)));
    TEST_ASSERT_TRUE(store.save(book));
    const CommandBook first = book;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 2, 9, id)));
    TEST_ASSERT_TRUE(store.save(book));
    TEST_ASSERT_EQUAL_UINT32(2, store.generation());

    slots.corrupt(1, 40);
    CommandStore reloaded(slots);
    CommandBook loaded{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LoadStatus::Loaded),
                          static_cast<int>(reloaded.load(loaded)));
    TEST_ASSERT_EQUAL_UINT32(1, reloaded.generation());
    TEST_ASSERT_TRUE(commandBooksEqual(first, loaded));

    slots.corrupt(0, 40);
    CommandStore broken(slots);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LoadStatus::Corrupt),
                          static_cast<int>(broken.load(loaded)));
}

void test_decode_rejects_invalid_snapshots() {
    CommandBook book = defaultCommandBook();
    uint16_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 1, 1, id)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(QueueStatus::Queued),
                          static_cast<int>(queueCount(book, 2, 1, id)));
    uint8_t valid[kCommandsSnapshotSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok),
                          static_cast<int>(encodeCommandBook(book, 1, valid, sizeof(valid))));
    CommandBook decoded{};
    uint32_t generation = 0;
    uint8_t snapshot[kCommandsSnapshotSize];

    memcpy(snapshot, valid, sizeof(snapshot));
    snapshot[16 + 24] = 1;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::CrcMismatch),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));
    resealCrc(snapshot);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::InvalidValue),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));

    memcpy(snapshot, valid, sizeof(snapshot));
    snapshot[16 + kStoredCommandSize + 10] = 1;
    resealCrc(snapshot);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::DuplicateValue),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));

    memcpy(snapshot, valid, sizeof(snapshot));
    snapshot[16 + 2 * kStoredCommandSize + 5] = 1;
    resealCrc(snapshot);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::InvalidReservedData),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));

    memcpy(snapshot, valid, sizeof(snapshot));
    protocol::writeUint16Le(snapshot + 14, 0);
    resealCrc(snapshot);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::InvalidValue),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));

    memcpy(snapshot, valid, sizeof(snapshot));
    snapshot[12] = kMaxCommandRecords + 1;
    resealCrc(snapshot);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CodecStatus::InvalidCount),
        static_cast<int>(decodeCommandBook(snapshot, sizeof(snapshot), decoded, generation)));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_command_store_loads_defaults);
    RUN_TEST(test_command_book_known_layout);
    RUN_TEST(test_queue_rejects_invalid_requests);
    RUN_TEST(test_read_info_queues_without_arguments_and_records_the_identity);
    RUN_TEST(test_complete_records_result_and_rejects_mismatches);
    RUN_TEST(test_record_of_a_previous_node_does_not_block_a_reused_id);
    RUN_TEST(test_command_ids_skip_zero_on_wrap);
    RUN_TEST(test_full_book_evicts_the_oldest_result);
    RUN_TEST(test_cancel_remove_and_clear);
    RUN_TEST(test_store_falls_back_to_the_previous_generation);
    RUN_TEST(test_decode_rejects_invalid_snapshots);
    return UNITY_END();
}
