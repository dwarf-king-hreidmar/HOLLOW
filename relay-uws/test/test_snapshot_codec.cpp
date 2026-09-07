// Unit tests for the restart snapshot codec (src/snapshot_codec.h): what the
// relay hands to systemd's fd store on SIGTERM and reads back on start.
//
// The property that matters is "exactly one snapshot or nothing": a round trip
// is byte-for-byte faithful, and every truncation, every bad byte and every
// foreign version is refused outright, because a partial restore would look
// like a relay that silently lost messages.
//
// Build + run from relay-uws/test (no uWebSockets, no libsodium):
//   g++ -std=c++17 -I../src test_snapshot_codec.cpp -o test_snapshot_codec && ./test_snapshot_codec

#include "snapshot_codec.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(const std::string& label, bool ok) {
    if (ok) {
        printf("  ok   %s\n", label.c_str());
    } else {
        printf("  FAIL %s\n", label.c_str());
        failures++;
    }
}

static snapshot::Data sample() {
    snapshot::Data d;
    snapshot::DmQueue q;
    q.target = "12D3KooWTargetOne";
    q.frames.push_back({"dmroom", std::string("\x06\x00binary\x00frame", 14), "12D3KooWSenderA", 120, false, false, 7});
    q.frames.push_back({"dmroom", "img", "12D3KooWSenderB", 3600, true, false, 9});
    q.frames.push_back({"srv:abc", "chan", "12D3KooWSenderA", 0, false, true, 11});
    d.dm.push_back(q);
    snapshot::DmQueue q2;
    q2.target = "12D3KooWTargetTwo";
    q2.frames.push_back({"dmroom2", std::string(1000, 'x'), "12D3KooWSenderB", 42, false, false, 8});
    d.dm.push_back(q2);

    d.optin.push_back({"12D3KooWTargetOne", 259200});
    d.optin.push_back({"12D3KooWTargetTwo", 3600});

    snapshot::Topic t;
    t.key = std::string("srv:abc\0general", 15);
    t.accepting = false;
    t.retention_secs = 86400;
    t.registered_age_secs = 5;
    t.frames.push_back({"f1", "12D3KooWSenderA", 10, 6});
    t.frames.push_back({"f2", "12D3KooWSenderB", 20, 10});
    d.topics.push_back(t);
    snapshot::Topic empty;
    empty.key = std::string("srv:abc\0quiet", 13);
    empty.retention_secs = 3600;
    d.topics.push_back(empty);

    d.push_tokens.push_back({"12D3KooWTargetOne", "fcm-token-xyz", "android"});
    d.push_tokens.push_back({"12D3KooWTargetTwo", "apns-token", "ios"});

    snapshot::PushPref p;
    p.peer = "12D3KooWTargetOne";
    snapshot::ServerPref s;
    s.server = "srv:abc";
    s.level = "mentions";
    s.channels.push_back({"general", "all"});
    s.channels.push_back({"noisy", "nothing"});
    p.servers.push_back(s);
    d.push_prefs.push_back(p);
    return d;
}

static bool same(const snapshot::Data& a, const snapshot::Data& b) {
    if (a.dm.size() != b.dm.size()) return false;
    for (size_t i = 0; i < a.dm.size(); i++) {
        if (a.dm[i].target != b.dm[i].target) return false;
        if (a.dm[i].frames.size() != b.dm[i].frames.size()) return false;
        for (size_t j = 0; j < a.dm[i].frames.size(); j++) {
            const auto& x = a.dm[i].frames[j];
            const auto& y = b.dm[i].frames[j];
            if (x.room != y.room || x.frame != y.frame || x.sender != y.sender ||
                x.age_secs != y.age_secs || x.is_image != y.is_image ||
                x.is_channel != y.is_channel || x.seq != y.seq) return false;
        }
    }
    if (a.optin.size() != b.optin.size()) return false;
    for (size_t i = 0; i < a.optin.size(); i++) {
        if (a.optin[i].peer != b.optin[i].peer || a.optin[i].retention_secs != b.optin[i].retention_secs) return false;
    }
    if (a.topics.size() != b.topics.size()) return false;
    for (size_t i = 0; i < a.topics.size(); i++) {
        const auto& x = a.topics[i];
        const auto& y = b.topics[i];
        if (x.key != y.key || x.accepting != y.accepting || x.retention_secs != y.retention_secs ||
            x.registered_age_secs != y.registered_age_secs || x.frames.size() != y.frames.size()) return false;
        for (size_t j = 0; j < x.frames.size(); j++) {
            if (x.frames[j].frame != y.frames[j].frame || x.frames[j].sender != y.frames[j].sender ||
                x.frames[j].age_secs != y.frames[j].age_secs || x.frames[j].seq != y.frames[j].seq) return false;
        }
    }
    if (a.push_tokens.size() != b.push_tokens.size()) return false;
    for (size_t i = 0; i < a.push_tokens.size(); i++) {
        if (a.push_tokens[i].peer != b.push_tokens[i].peer || a.push_tokens[i].token != b.push_tokens[i].token ||
            a.push_tokens[i].platform != b.push_tokens[i].platform) return false;
    }
    if (a.push_prefs.size() != b.push_prefs.size()) return false;
    for (size_t i = 0; i < a.push_prefs.size(); i++) {
        const auto& x = a.push_prefs[i];
        const auto& y = b.push_prefs[i];
        if (x.peer != y.peer || x.servers.size() != y.servers.size()) return false;
        for (size_t j = 0; j < x.servers.size(); j++) {
            if (x.servers[j].server != y.servers[j].server || x.servers[j].level != y.servers[j].level ||
                x.servers[j].channels.size() != y.servers[j].channels.size()) return false;
            for (size_t k = 0; k < x.servers[j].channels.size(); k++) {
                if (x.servers[j].channels[k].channel != y.servers[j].channels[k].channel ||
                    x.servers[j].channels[k].level != y.servers[j].channels[k].level) return false;
            }
        }
    }
    return true;
}

int main() {
    printf("snapshot codec\n");

    // Round trip: everything comes back, including embedded NULs and the
    // empty ring.
    {
        snapshot::Data in = sample();
        std::string bytes = snapshot::encode(in);
        snapshot::Data out;
        check("decode succeeds", snapshot::decode(bytes, out));
        check("round trip is faithful", same(in, out));
        check("frame counts", out.dm_frames() == 4 && out.topic_frames() == 2);
        check("re-encode is byte-identical", snapshot::encode(out) == bytes);
    }

    // An empty relay is a valid snapshot too.
    {
        snapshot::Data in;
        std::string bytes = snapshot::encode(in);
        snapshot::Data out;
        check("empty snapshot decodes", snapshot::decode(bytes, out));
        check("empty snapshot is empty", out.dm.empty() && out.optin.empty() && out.topics.empty() &&
                                         out.push_tokens.empty() && out.push_prefs.empty());
    }

    // Every proper prefix is refused and leaves `out` untouched.
    {
        std::string bytes = snapshot::encode(sample());
        bool all_refused = true;
        bool untouched = true;
        for (size_t n = 0; n < bytes.size(); n++) {
            snapshot::Data out;
            out.optin.push_back({"sentinel", 1});
            if (snapshot::decode(std::string_view(bytes).substr(0, n), out)) all_refused = false;
            if (out.optin.size() != 1 || out.optin[0].peer != "sentinel") untouched = false;
        }
        check("every truncation is refused", all_refused);
        check("a refused decode leaves the output untouched", untouched);
    }

    // Trailing bytes, a foreign version, a bad magic, a bad flag byte and an
    // impossible count are all refused.
    {
        std::string bytes = snapshot::encode(sample());
        snapshot::Data out;
        check("trailing garbage is refused", !snapshot::decode(bytes + "x", out));

        std::string wrong_version = bytes;
        wrong_version[4] = static_cast<char>(snapshot::VERSION + 1);
        check("a foreign version is refused", !snapshot::decode(wrong_version, out));

        std::string bad_magic = bytes;
        bad_magic[0] = 'X';
        check("a bad magic is refused", !snapshot::decode(bad_magic, out));

        // The first DM frame's is_image flag sits right after its age field;
        // locate it by searching for the sender that precedes the age.
        std::string bad_flag = bytes;
        size_t sender_at = bad_flag.find("12D3KooWSenderA");
        size_t flag_at = sender_at + std::string("12D3KooWSenderA").size() + 4;
        bad_flag[flag_at] = 2;
        check("a flag byte other than 0/1 is refused", !snapshot::decode(bad_flag, out));

        // Corrupt the top-level DM queue count to something larger than the
        // whole snapshot.
        std::string bad_count = bytes;
        bad_count[8] = static_cast<char>(0xff);
        bad_count[9] = static_cast<char>(0xff);
        bad_count[10] = static_cast<char>(0xff);
        bad_count[11] = static_cast<char>(0x7f);
        check("a count past the end is refused", !snapshot::decode(bad_count, out));
    }

    // A string length past the ceiling is refused before any allocation.
    {
        std::string bytes = snapshot::encode(sample());
        // First string is the first DM target: its length field is at offset 12.
        bytes[12] = static_cast<char>(0x01);
        bytes[13] = static_cast<char>(0x00);
        bytes[14] = static_cast<char>(0x00);
        bytes[15] = static_cast<char>(0x08);  // 128 MB + 1
        snapshot::Data out;
        check("a string past the ceiling is refused", !snapshot::decode(bytes, out));
    }

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all ok\n");
    return 0;
}
