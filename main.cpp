#include <bits/stdc++.h>
#include <sys/stat.h>
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
const char* BUCKETS_FILE = "buckets.dat";
const char* ENTRIES_FILE = "entries.dat";

vector<int64_t> buckets;
FILE* f_buckets = nullptr;
FILE* f_entries = nullptr;
int64_t next_free_offset = 0;

static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline int hash_key(const char* key, int len) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < len; i++) {
        h ^= static_cast<uint8_t>(key[i]);
        h *= 1099511628211ULL;
    }
    return static_cast<int>(splitmix64(h) % B);
}

static inline bool key_equals(const Entry& e, const char* key, int len) {
    return e.key_len == static_cast<uint8_t>(len) && memcmp(e.key, key, len) == 0;
}

void init_files() {
    struct stat st;
    bool buckets_ok = (stat(BUCKETS_FILE, &st) == 0 && st.st_size == static_cast<off_t>(B * 8LL));
    bool entries_ok = (stat(ENTRIES_FILE, &st) == 0);

    if (!buckets_ok) {
        f_buckets = fopen(BUCKETS_FILE, "wb");
        static int64_t zeros[4096];
        memset(zeros, 0, sizeof(zeros));
        int remaining = B;
        while (remaining > 0) {
            int batch = min(remaining, static_cast<int>(sizeof(zeros) / 8));
            fwrite(zeros, 8, batch, f_buckets);
            remaining -= batch;
        }
        fclose(f_buckets);
    }

    f_buckets = fopen(BUCKETS_FILE, "r+b");
    buckets.resize(B);
    rewind(f_buckets);
    fread(buckets.data(), 8, B, f_buckets);

    if (!entries_ok) {
        f_entries = fopen(ENTRIES_FILE, "w+b");
    } else {
        f_entries = fopen(ENTRIES_FILE, "r+b");
    }
    fseek(f_entries, 0, SEEK_END);
    next_free_offset = ftell(f_entries);
    if (next_free_offset == 0) {
        // Reserve offset 0 as null sentinel; first real entry starts at sizeof(Entry).
        next_free_offset = sizeof(Entry);
    }
}

static inline void write_bucket(int idx) {
    fseek(f_buckets, idx * 8LL, SEEK_SET);
    fwrite(&buckets[idx], 8, 1, f_buckets);
}

static inline int64_t append_entry(const Entry& e) {
    int64_t off = next_free_offset;
    fseek(f_entries, off, SEEK_SET);
    fwrite(&e, sizeof(Entry), 1, f_entries);
    next_free_offset += sizeof(Entry);
    return off;
}

static inline void read_entry(int64_t off, Entry& e) {
    fseek(f_entries, off, SEEK_SET);
    fread(&e, sizeof(Entry), 1, f_entries);
}

static inline void write_entry(int64_t off, const Entry& e) {
    fseek(f_entries, off, SEEK_SET);
    fwrite(&e, sizeof(Entry), 1, f_entries);
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
    write_bucket(h);
}

void do_delete(const char* key, int len, int value) {
    int h = hash_key(key, len);
    int64_t off = buckets[h];
    int64_t prev = -1;
    Entry e;
    while (off != 0) {
        read_entry(off, e);
        if (!e.deleted && key_equals(e, key, len) && e.value == value) {
            if (prev == -1) {
                buckets[h] = e.next;
                write_bucket(h);
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
        if (!e.deleted && key_equals(e, key, len)) {
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

    if (f_buckets) fclose(f_buckets);
    if (f_entries) fclose(f_entries);
    return 0;
}
