// Recipe step world-levels (recipe 1.20.0): register the MP world levels.     u3fix world-levels --project P [--apply]
//
// A PS3 multiplayer map whose second stage streams in (London Underground: the train arrives at the station) needs its
// world level: world-london-mp-ingame12.pak holds the region tree rgn-ldn-world-1, which loads ldn-train-multi (the
// station) when the train gets there. The level list of the train map in net.bin does not name ldn-train-multi, so
// without the world level the station never loads: no geometry, no collision, every respawn falls into the void.
//
// convert-levels-r3 builds the pak (plan.tsv row world-london-mp-ingame12). The registry steps (registry-levels,
// registry-final) write the reference pak23_ps4.txt, which has no block for it, so this step adds
//   level-name world-london-mp / level-id world-london-mp / package world-london-mp-ingame
// to overlay-r3's pak23_ps4.txt, right after the world-grave-mp block (else at the end), when the pak is in the overlay.
// Dry run by default; --apply writes (pak23_ps4.txt backed up once to work/backup-world-levels/).
// Idempotent: prints 'already applied' and exits 0 when every world level whose pak is present is registered.
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {
namespace {

struct WorldLevel {
    const char *level;     // level-name / level-id
    const char *package;   // package line (the pak is <package>12.pak)
};

const WorldLevel LEVELS[] = {
    {"world-london-mp", "world-london-mp-ingame"},
};

const char *const ANCHOR = "world-grave-mp";   // insert after this level's block

bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// second word of a line whose first word is 'level-name', else ""
std::string level_name(const std::string &ln) {
    size_t i = 0;
    while (i < ln.size() && space(ln[i])) i++;
    const std::string key = "level-name";
    if (ln.compare(i, key.size(), key) != 0) return "";
    i += key.size();
    if (i >= ln.size() || !space(ln[i])) return "";
    while (i < ln.size() && space(ln[i])) i++;
    size_t j = i;
    while (j < ln.size() && !space(ln[j])) j++;
    return ln.substr(i, j - i);
}

std::vector<std::string> split_lines(const std::string &text, const std::string &nl) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (true) {
        size_t e = text.find(nl, pos);
        v.push_back(text.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + nl.size();
    }
    return v;
}

}  // namespace

int world_levels(const std::vector<std::string> &argv) {
    Args a(argv, {"--apply"}, {"--project"});
    std::string project = a.need("--project");
    fs::path dir = upath(project + "/work/overlay-r3/u3data/build/main/");
    fs::path reg = dir / "pak23_ps4.txt";
    fs::path bk = upath(project + "/work/backup-world-levels/");
    std::error_code ec;
    if (!fs::is_regular_file(reg, ec)) {
        std::printf("overlay-r3 pak23_ps4.txt not found: nothing to do yet\n");
        return 0;
    }
    Bytes b = read_file(reg);
    std::string text(b.begin(), b.end());
    std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines = split_lines(text, nl);

    std::vector<std::string> added;
    for (const auto &w : LEVELS) {
        if (!fs::is_regular_file(dir / upath(std::string("pak23/ps4/") + w.package + "12.pak"), ec)) {
            std::printf("%s: %s12.pak not in overlay-r3, not registered\n", w.level, w.package);
            continue;
        }
        bool have = false;
        for (const auto &ln : lines)
            if (level_name(ln) == w.level) { have = true; break; }
        if (have) continue;
        // end of the anchor block = the next level-name line after it (or the end of the text)
        size_t at = lines.size();
        while (at > 0 && lines[at - 1].empty()) at--;   // keep a trailing newline after the block
        for (size_t i = 0; i < lines.size(); i++) {
            if (level_name(lines[i]) != ANCHOR) continue;
            size_t j = i + 1;
            while (j < lines.size() && level_name(lines[j]).empty()) j++;
            if (j < lines.size()) at = j;
            break;
        }
        std::vector<std::string> block = {std::string("level-name ") + w.level, std::string("level-id ") + w.level,
                                          std::string("package ") + w.package};
        lines.insert(lines.begin() + at, block.begin(), block.end());
        added.push_back(w.level);
    }
    if (added.empty()) {
        std::printf("already applied: every world level present in overlay-r3 is registered\n");
        return 0;
    }
    for (const auto &l : added) std::printf("overlay-r3 pak23_ps4.txt: + %s\n", l.c_str());
    if (!a.has("--apply")) {
        std::printf("dry run: nothing changed\n");
        return 0;
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += (i ? nl : "") + lines[i];
    fs::create_directories(bk);
    copy_once(reg, bk / "overlay-r3-pak23_ps4.txt");
    write_file_replace(reg, Bytes(out.begin(), out.end()));
    std::printf("applied: %d world level(s) registered\n", int(added.size()));
    return 0;
}

}  // namespace u3
