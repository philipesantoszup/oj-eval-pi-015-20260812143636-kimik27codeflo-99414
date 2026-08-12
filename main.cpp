#include <bits/stdc++.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <random>
using namespace std;

#pragma pack(push, 1)
struct Entry {
    int64_t next;
    int32_t value;
    char key[64];
    uint8_t key_len;
    uint8_t deleted;
    uint8_t padding[2];
};
#pragma pack(pop)

static_assert(sizeof(Entry) == 80, "Entry size must be 80");

constexpr int B = 65537;
constexpr int64_t BUCKETS_HEADER_SIZE = 8; // seed
constexpr int64_t BUCKETS_DATA_SIZE = B * 8LL;
constexpr int64_t BUCKETS_FILE_SIZE = BUCKETS_HEADER_SIZE + BUCKETS_DATA_SIZE;
const char* BUCKETS_FILE = "buckets.dat";
const char* ENTRIES_FILE = "entries.dat";

int fd_buckets = -1;
int fd_entries = -1;
vector<int64_t> buckets;
uint64_t hash_seed = 0;
int64_t next_free_offset = 0;

static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline int hash_key(const char* key, int len) {
    uint64_t h = hash_seed;
    for (int i = 0; i < len; i++) {
        h ^= static_cast<uint8_t>(key[i]);
        h *= 1099511628211ULL;
    }
    return static_cast<int>(splitmix64(h) % B);
}

static inline bool key_equals(const Entry& e, const char* key, int len) {
    return e.key_len == static_cast<uint8_t>(len) && memcmp(e.key, key, len) == 0;
}

static inline void full_pread(int fd, void* buf, size_t count, off_t offset) {
    char* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < count) {
        ssize_t r = pread(fd, p + done, count - done, offset + static_cast<off_t>(done));
        if (r <= 0) break;
        done += r;
    }
}

static inline void full_pwrite(int fd, const void* buf, size_t count, off_t offset) {
    const char* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < count) {
        ssize_t r = pwrite(fd, p + done, count - done, offset + static_cast<off_t>(done));
        if (r < 0) break;
        done += r;
    }
}

static uint64_t generate_seed() {
    std::random_device rd;
    if (rd.entropy() > 0) {
        uint64_t a = rd();
        uint64_t b = rd();
        return (a << 32) ^ b;
    }
    return static_cast<uint64_t>(chrono::steady_clock::now().time_since_epoch().count());
}

void init_files() {
    struct stat st;
    bool buckets_ok = (stat(BUCKETS_FILE, &st) == 0 && st.st_size == static_cast<off_t>(BUCKETS_FILE_SIZE));
    bool entries_ok = (stat(ENTRIES_FILE, &st) == 0);

    if (!buckets_ok) {
        fd_buckets = open(BUCKETS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        hash_seed = generate_seed();
        full_pwrite(fd_buckets, &hash_seed, sizeof(hash_seed), 0);
        static int64_t zeros[4096];
        memset(zeros, 0, sizeof(zeros));
        int64_t remaining = BUCKETS_DATA_SIZE;
        int64_t off = BUCKETS_HEADER_SIZE;
        while (remaining > 0) {
            size_t batch = min(remaining, static_cast<int64_t>(sizeof(zeros)));
            full_pwrite(fd_buckets, zeros, batch, off);
            off += batch;
            remaining -= batch;
        }
        close(fd_buckets);
    }

    fd_buckets = open(BUCKETS_FILE, O_RDWR, 0666);
    full_pread(fd_buckets, &hash_seed, sizeof(hash_seed), 0);
    buckets.resize(B);
    full_pread(fd_buckets, buckets.data(), BUCKETS_DATA_SIZE, BUCKETS_HEADER_SIZE);

    if (!entries_ok) {
        fd_entries = open(ENTRIES_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    } else {
        fd_entries = open(ENTRIES_FILE, O_RDWR, 0666);
    }
    next_free_offset = lseek(fd_entries, 0, SEEK_END);
    if (next_free_offset == 0) {
        next_free_offset = sizeof(Entry);
    }
}

static inline void sync_buckets() {
    full_pwrite(fd_buckets, &hash_seed, sizeof(hash_seed), 0);
    full_pwrite(fd_buckets, buckets.data(), BUCKETS_DATA_SIZE, BUCKETS_HEADER_SIZE);
}

static inline int64_t append_entry(const Entry& e) {
    int64_t off = next_free_offset;
    full_pwrite(fd_entries, &e, sizeof(Entry), off);
    next_free_offset += sizeof(Entry);
    return off;
}

static inline void read_entry(int64_t off, Entry& e) {
    full_pread(fd_entries, &e, sizeof(Entry), off);
}

static inline void write_entry(int64_t off, const Entry& e) {
    full_pwrite(fd_entries, &e, sizeof(Entry), off);
}

void do_insert(const char* key, int len, int value) {
    int h = hash_key(key, len);
    int64_t off = buckets[h];
    Entry e;
    while (off != 0) {
        read_entry(off, e);
        if (key_equals(e, key, len) && e.value == value) return;
        off = e.next;
    }
    e.next = buckets[h];
    e.value = value;
    memcpy(e.key, key, len);
    if (len < 64) memset(e.key + len, 0, 64 - len);
    e.key_len = static_cast<uint8_t>(len);
    e.deleted = 0;
    e.padding[0] = e.padding[1] = 0;
    off = append_entry(e);
    buckets[h] = off;
}

void do_delete(const char* key, int len, int value) {
    int h = hash_key(key, len);
    int64_t off = buckets[h];
    int64_t prev = -1;
    Entry e;
    while (off != 0) {
        read_entry(off, e);
        if (key_equals(e, key, len) && e.value == value) {
            if (prev == -1) {
                buckets[h] = e.next;
            } else {
                Entry prev_e;
                read_entry(prev, prev_e);
                prev_e.next = e.next;
                write_entry(prev, prev_e);
            }
            return;
        }
        prev = off;
        off = e.next;
    }
}

void do_find(const char* key, int len) {
    int h = hash_key(key, len);
    int64_t off = buckets[h];
    Entry e;
    static vector<int32_t> values;
    values.clear();
    while (off != 0) {
        read_entry(off, e);
        if (key_equals(e, key, len)) {
            values.push_back(e.value);
        }
        off = e.next;
    }
    sort(values.begin(), values.end());
    if (values.empty()) {
        printf("null\n");
    } else {
        size_t n = values.size();
        for (size_t i = 0; i < n; i++) {
            if (i) putchar(' ');
            printf("%d", values[i]);
        }
        putchar('\n');
    }
}

int main() {
    init_files();

    int n;
    scanf("%d", &n);
    char cmd[16];
    char key[80];
    int value;
    for (int i = 0; i < n; i++) {
        scanf("%s", cmd);
        if (cmd[0] == 'i') {
            scanf("%s%d", key, &value);
            do_insert(key, static_cast<int>(strlen(key)), value);
        } else if (cmd[0] == 'd') {
            scanf("%s%d", key, &value);
            do_delete(key, static_cast<int>(strlen(key)), value);
        } else {
            scanf("%s", key);
            do_find(key, static_cast<int>(strlen(key)));
        }
    }

    sync_buckets();
    if (fd_buckets >= 0) close(fd_buckets);
    if (fd_entries >= 0) close(fd_entries);
    return 0;
}
