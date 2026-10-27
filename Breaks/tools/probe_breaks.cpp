// What Breaks makes of a folder of WAVs, using the plugin's own scanner, loader and tempo guess:
//   probe_breaks <folder> [media root]     (built by probe_breaks.sh)
// One line per file: name | rate | seconds | where the length came from | beats | BPM, or the load error.
#include <cstdio>

#include "../BreakLibrary.h"
#include "../WavFile.h"

int main(int argc, char **argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "usage: %s <folder>\n", argv[0]);
        return 2;
    }
    auto list = breaklib::Scan(argv[1], argc > 2 ? argv[2] : "");   // [media root]: the drives too
    int  bad  = 0;
    for(auto &e : list)
    {
        WavData     w;
        const char *err = wavfile::Load(e.path.c_str(), w);
        if(*err)
        {
            printf("%-60.60s | ERROR %s\n", e.name.c_str(), err);
            bad++;
            continue;
        }
        double      secs = w.frames / w.sampleRate;
        float       bpm  = 0;
        const char *src  = "";
        int         beats = breaklib::GuessBeats(secs, w.acidBeats, w.acidTempo, e.name, &bpm, &src);
        printf("%-60.60s | %5.0f Hz | %6.2f s | %-9s | %3d beats (%5.2f bars) | %6.1f BPM\n", e.name.c_str(),
               w.sampleRate, secs, src, beats, beats / 4.0, bpm);
    }
    printf("%zu files, %d unreadable\n", list.size(), bad);
    return 0;
}
