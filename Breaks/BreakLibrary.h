#pragma once
// ─── BreakLibrary.h ───────────────────────────────────────────────────────────
// Finding break WAVs and working out how long they are in bars. Worker thread only.
//
// Scanned, 3 levels deep, symlinks not followed:
//   1. the plugin's own folder, <plugin folder>/breaks (MODULE_SUBDIR): the curated set, listed first, so
//      its indexes stay put when a drive's folders change. The installer keeps it on upgrade (vst.json
//      "user_data").
//   2. any folder with "break" in its name on a drive (/media/<drive>/<folder>, or one level further
//      down), e.g. /media/MPCDATA/Breaks; listed after, each under its folder name ("Breaks/...").
//      Plugin folders (Synths) are skipped, and a file reached twice is listed once.
// Each part is sorted by name, so an index means the same file until files are added; projects save
// the chosen break by name.
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

struct BreakEntry
{
    std::string path;   // absolute
    std::string name;   // display name: path below the scanned folder, without ".wav"
};

namespace breaklib
{
constexpr size_t kMaxEntries = 2000;   // params.json "break" max + 1

inline bool IsWav(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && n[0] != '.' && !strcasecmp(n + l - 4, ".wav");
}

inline void Walk(const std::string &dir, const std::string &prefix, int depth,
                 std::vector<BreakEntry> &out, std::vector<std::pair<dev_t, ino_t>> &seen)
{
    DIR *d = opendir(dir.c_str());
    if(!d)
        return;
    std::vector<std::string> subdirs;
    while(dirent *de = readdir(d))
    {
        if(de->d_name[0] == '.' || out.size() >= kMaxEntries)
            continue;
        std::string path = dir + "/" + de->d_name;
        struct stat st;
        if(lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
            continue;
        if(S_ISDIR(st.st_mode))
        {
            if(depth > 1)
                subdirs.push_back(de->d_name);
        }
        else if(S_ISREG(st.st_mode) && IsWav(de->d_name))
        {
            auto id = std::make_pair(st.st_dev, st.st_ino);
            if(std::find(seen.begin(), seen.end(), id) != seen.end())
                continue;
            seen.push_back(id);
            std::string n = prefix + de->d_name;
            out.push_back({path, n.substr(0, n.size() - 4)});
        }
    }
    closedir(d);
    for(auto &s : subdirs)
        Walk(dir + "/" + s, prefix + s + "/", depth - 1, out, seen);
}

inline bool HasBreak(const char *n)
{
    for(const char *p = n; *p; p++)
        if(!strncasecmp(p, "break", 5))
            return true;
    return false;
}

// Folders under `dir` (and one level below) with "break" in the name, as (path, name below `dir`).
inline void FindBreakFolders(const std::string &dir, const std::string &prefix, int depth,
                             std::vector<std::pair<std::string, std::string>> &out)
{
    DIR *d = opendir(dir.c_str());
    if(!d)
        return;
    std::vector<std::string> names;
    while(dirent *de = readdir(d))
        if(de->d_name[0] != '.' && strcasecmp(de->d_name, "Synths") && strcmp(de->d_name, "lost+found"))
            names.push_back(de->d_name);
    closedir(d);
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    for(auto &n : names)
    {
        std::string path = dir + "/" + n;
        struct stat st;
        if(lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if(HasBreak(n.c_str()))
            out.push_back({path, prefix + n});
        else if(depth > 1)
            FindBreakFolders(path, prefix + n + "/", depth - 1, out);
    }
}

inline void SortByName(std::vector<BreakEntry> &v, size_t from)
{
    std::sort(v.begin() + from, v.end(), [](const BreakEntry &a, const BreakEntry &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

// `mediaRoot`: where the drives are mounted ("" = none).
inline std::vector<BreakEntry> Scan(const std::string &pluginDataDir, const std::string &mediaRoot = "/media")
{
    std::vector<BreakEntry>              out;
    std::vector<std::pair<dev_t, ino_t>> seen;
    if(!pluginDataDir.empty())
        Walk(pluginDataDir, "", 3, out, seen);
    SortByName(out, 0);
    if(mediaRoot.empty())
        return out;
    std::vector<std::pair<std::string, std::string>> folders;
    DIR *d = opendir(mediaRoot.c_str());
    if(!d)
        return out;
    std::vector<std::string> drives;
    while(dirent *de = readdir(d))
        if(de->d_name[0] != '.')
            drives.push_back(de->d_name);
    closedir(d);
    std::sort(drives.begin(), drives.end());
    for(auto &dr : drives)
        FindBreakFolders(mediaRoot + "/" + dr, "", 2, folders);
    for(auto &f : folders)
    {
        // the name shown is the break folder's own name and below: "Breaks/All The Breaks/Cold Sweat"
        size_t      slash = f.second.rfind('/');
        std::string label = slash == std::string::npos ? f.second : f.second.substr(slash + 1);
        size_t      from  = out.size();
        Walk(f.first, label + "/", 3, out, seen);
        SortByName(out, from);
    }
    return out;
}

// A tempo written in the file name: a number 60-200 next to "bpm" wins, else the
// first standalone 60-200 number ("Amen 136", "funky_drummer_102_4bar").
inline float BpmFromName(const std::string &name)
{
    std::string s = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
    float       loose = 0;
    for(size_t i = 0; i < s.size();)
    {
        if(!isdigit((unsigned char)s[i]))
        {
            i++;
            continue;
        }
        size_t j = i;
        while(j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '.'))
            j++;
        float v      = (float)atof(s.substr(i, j - i).c_str());
        bool  bpmTag = (j + 3 <= s.size() && !strncasecmp(s.c_str() + j, "bpm", 3))
                      || (j + 4 <= s.size() && !strncasecmp(s.c_str() + j, " bpm", 4));
        bool standalone = (i == 0 || !isalpha((unsigned char)s[i - 1]))
                          && (j == s.size() || !isalpha((unsigned char)s[j]) || bpmTag);
        if(v >= 60 && v <= 200)
        {
            if(bpmTag)
                return v;
            if(standalone && loose == 0)
                loose = v;
        }
        i = j;
    }
    return loose;
}

// A bar count written in the file name: "4bar", "2 bars", "Amen_8BAR" (1-64).
inline int BarsFromName(const std::string &name)
{
    std::string s = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
    for(size_t i = 0; i < s.size(); i++)
    {
        if(!isdigit((unsigned char)s[i]) || (i > 0 && isdigit((unsigned char)s[i - 1])))
            continue;
        size_t j = i;
        while(j < s.size() && isdigit((unsigned char)s[j]))
            j++;
        size_t k = j;
        while(k < s.size() && (s[k] == ' ' || s[k] == '-' || s[k] == '_'))
            k++;
        if(k + 3 <= s.size() && !strncasecmp(s.c_str() + k, "bar", 3)
           && (k + 3 == s.size() || !isalpha((unsigned char)s[k + 3]) || tolower(s[k + 3]) == 's'))
        {
            int n = atoi(s.substr(i, j - i).c_str());
            if(n >= 1 && n <= 64)
                return n;
        }
    }
    return 0;
}

// The name for the readout: the file's own name, without its folders (a drive folder's path would not fit) and
// with the "4bar" tag taken out (the readout shows the length anyway). Display only: saving and lookup use the
// real name.
inline std::string DisplayName(const std::string &name)
{
    size_t      slash = name.rfind('/');
    std::string s     = slash == std::string::npos ? name : name.substr(slash + 1);
    for(size_t i = 0; i < s.size(); i++)
    {
        if(!isdigit((unsigned char)s[i]) || (i > 0 && isdigit((unsigned char)s[i - 1])))
            continue;
        size_t k = i;
        while(k < s.size() && isdigit((unsigned char)s[k]))
            k++;
        while(k < s.size() && (s[k] == ' ' || s[k] == '-' || s[k] == '_'))
            k++;
        if(k + 3 <= s.size() && !strncasecmp(s.c_str() + k, "bar", 3)
           && (k + 3 == s.size() || !isalpha((unsigned char)s[k + 3]) || tolower(s[k + 3]) == 's'))
        {
            size_t end = k + 3 + (k + 3 < s.size() && tolower(s[k + 3]) == 's');
            size_t beg = i;
            while(beg > 0 && (s[beg - 1] == ' ' || s[beg - 1] == '_' || s[beg - 1] == '-'))
                beg--;
            s.erase(beg, end - beg);
            break;
        }
    }
    return s.empty() ? name : s;
}

// How many beats a loop of `seconds` is, the tempo that implies, and where that came from.
// Order of trust:
//   1. a bar count in the name ("4bar")
//   2. the ACID chunk, only with a real tempo matching beats/length within 1% (tools that tag files
//      without knowing the tempo write 0 and a beat count computed at 120 BPM)
//   3. a tempo in the name, rounded to whole beats
//   4. the power-of-two bar count that puts the tempo nearest 110 within 70-180 BPM
// Breaks need not be whole bars: a 13-beat loop is sliced into 52 sixteenths.
inline int GuessBeats(double seconds, int acidBeats, float acidTempo, const std::string &name, float *bpmOut,
                      const char **sourceOut = nullptr)
{
    int         beats = 0;
    const char *src   = "length";
    if(int bars = BarsFromName(name))
        beats = bars * 4, src = "name bars";
    else if(acidBeats >= 1 && acidTempo > 0 && fabs(acidBeats * 60.0 / seconds - acidTempo) < acidTempo * 0.01)
        beats = acidBeats, src = "acid";
    else if(float nameBpm = BpmFromName(name))
        beats = (int)lround(seconds * nameBpm / 60.0), src = "name bpm";
    else
    {
        double best = 1e9;
        for(int b = 1; b <= 64; b *= 2)
        {
            double bpm = b * 240.0 / seconds;
            double err = (bpm >= 70 && bpm <= 180) ? fabs(bpm - 110) : 1000 + fabs(bpm - 110);
            if(err < best)
                best = err, beats = b * 4;
        }
    }
    if(beats < 1)
        beats = 1;
    if(bpmOut)
        *bpmOut = (float)(beats * 60.0 / seconds);
    if(sourceOut)
        *sourceOut = src;
    return beats;
}
} // namespace breaklib
