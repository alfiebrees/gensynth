#include "PluginProcessor.h"
#include <cmath>
#include <regex>
#include <thread>

static constexpr double kGenRate = 44100.0;
static constexpr double kTwoPi   = 6.283185307179586;

using Buf = juce::AudioBuffer<float>;

//==============================================================================
// Sample bank building: synthesized multisamples (generated once, shared by
// every instance of the plugin) + optional user WAV folders
//==============================================================================

// Adds one decaying / wobbling sine partial using a rotating phasor (fast).
static void addPartial (float* dst, int n, double sr, double freq, double amp,
                        double tau, double phase,
                        double lfoHz = 0.0, double lfoDepth = 0.0, double lfoPhase = 0.0)
{
    if (freq <= 0.0 || freq >= sr * 0.45)
        return;

    const double w = kTwoPi * freq / sr;
    double c = std::cos (phase), s = std::sin (phase);
    const double cd = std::cos (w), sd = std::sin (w);
    double a = amp;
    const double am = tau > 0.0 ? std::exp (-1.0 / (tau * sr)) : 1.0;

    const bool useLfo = lfoHz > 0.0 && lfoDepth > 0.0;
    const double lw = kTwoPi * lfoHz / sr;
    double lc = std::cos (lfoPhase), ls = std::sin (lfoPhase);
    const double lcd = std::cos (lw), lsd = std::sin (lw);

    for (int i = 0; i < n; ++i)
    {
        double v = s * a;
        if (useLfo)
        {
            v *= 1.0 - lfoDepth * 0.5 * (1.0 - ls);
            const double nls = ls * lcd + lc * lsd;
            const double nlc = lc * lcd - ls * lsd;
            ls = nls; lc = nlc;
        }
        dst[i] += (float) v;

        const double ns = s * cd + c * sd;
        const double nc = c * cd - s * sd;
        s = ns; c = nc;
        a *= am;
    }
}

static void makeLoop (Buf& b, int length, int loopStart, int xf)
{
    xf = juce::jmin (xf, loopStart, length - loopStart);
    if (xf < 2)
        return;

    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int k = 0; k < xf; ++k)
        {
            const float t = ((float) k + 0.5f) / (float) xf;
            const int idx = length - xf + k;
            d[idx] = d[idx] * (1.0f - t) + d[loopStart - xf + k] * t;
        }
        for (int k = 0; k < 4; ++k)
            d[length + k] = d[loopStart + k];
    }
}

static void finishBuffer (Buf& b, int length, bool loop, int loopStart, int xf)
{
    float mx = 0.0f;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* d = b.getReadPointer (ch);
        for (int i = 0; i < length; ++i)
            mx = juce::jmax (mx, std::abs (d[i]));
    }
    if (mx > 1.0e-6f)
        b.applyGain (0, length, 0.7f / mx);

    const int fi = juce::jmin (length, loop ? 400 : 64);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int i = 0; i < fi; ++i)
            d[i] *= (float) i / (float) fi;
    }

    if (loop)
    {
        makeLoop (b, length, loopStart, xf);
    }
    else
    {
        const int fo = juce::jmin (length, (int) (0.2 * kGenRate));
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            float* d = b.getWritePointer (ch);
            for (int i = 0; i < fo; ++i)
                d[length - fo + i] *= (float) (fo - i) / (float) fo;
        }
    }
}

//------------------------------------------------------------------ generators
// 0: rhodes (FM electric piano, mono)
static void genRhodes (Buf& b, double f, int midi, int N)
{
    float* d = b.getWritePointer (0);
    const double sr = kGenRate;
    const double tauBody = juce::jlimit (0.6, 4.0, 2.8 * std::pow (2.0, -(midi - 48) / 30.0));
    const double ratio = (f * 14.0 < 15000.0) ? 14.0 : 4.0;
    const double w = kTwoPi * f / sr;

    for (int i = 0; i < N; ++i)
    {
        const double t = (double) i / sr;
        const double env = std::exp (-t / tauBody) * (1.0 - std::exp (-t / 0.0015));
        const double idxBody = 1.1 * std::exp (-t / 0.45) + 0.2;
        const double ph = w * (double) i;
        const double body = std::sin (ph + idxBody * std::sin (ph));
        const double h2   = 0.22 * std::sin (2.0 * ph) * std::exp (-t / (tauBody * 0.5));
        const double tine = 0.28 * std::exp (-t / 0.3)
                          * std::sin (ph + 2.4 * std::exp (-t / 0.05) * std::sin (ratio * ph));
        d[i] = (float) ((0.9 * body + h2 + tine) * env);
    }
}

// 1: dream pad (detuned, slowly breathing harmonics, stereo)
static void genPad (Buf& b, double f, int N, int seed)
{
    juce::Random rng (seed);
    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = b.getWritePointer (ch);
        const double fch = f * std::pow (2.0, (ch == 0 ? -6.0 : 6.0) / 1200.0);
        for (int n = 1; n <= 24; ++n)
        {
            const double freq = fch * n;
            if (freq > 0.42 * kGenRate) break;
            addPartial (d, N, kGenRate, freq, 1.0 / std::pow ((double) n, 1.35), 0.0,
                        rng.nextDouble() * kTwoPi,
                        0.12 + 0.05 * (n % 5), 0.35, rng.nextDouble() * kTwoPi);
        }
        addPartial (d, N, kGenRate, f * 0.5, 0.35, 0.0, rng.nextDouble() * kTwoPi);
    }
}

// 2: air choir ("ah" formants + breath, stereo ensemble)
static void genChoir (Buf& b, double f, int N, int seed)
{
    juce::Random rng (seed);
    auto formant = [] (double hz)
    {
        auto g = [] (double x, double c, double bw) { const double z = (x - c) / bw; return std::exp (-0.5 * z * z); };
        return g (hz, 700.0, 130.0) + 0.55 * g (hz, 1150.0, 160.0) + 0.25 * g (hz, 2600.0, 300.0) + 0.04;
    };
    const double cents[2][2] = { { -9.0, 4.0 }, { -4.0, 9.0 } };

    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int set = 0; set < 2; ++set)
        {
            const double fs = f * std::pow (2.0, cents[ch][set] / 1200.0);
            for (int n = 1; n <= 24; ++n)
            {
                const double freq = fs * n;
                if (freq > 0.42 * kGenRate) break;
                addPartial (d, N, kGenRate, freq, formant (freq) / std::pow ((double) n, 0.35), 0.0,
                            rng.nextDouble() * kTwoPi,
                            0.2 + 0.07 * (n % 4), 0.3, rng.nextDouble() * kTwoPi);
            }
        }
        float lp = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float nz = rng.nextFloat() * 2.0f - 1.0f;
            lp += 0.3f * (nz - lp);
            d[i] += 0.2f * lp;
        }
    }
}

// 3: glass bell (inharmonic partials, stereo)
static void genBell (Buf& b, double f, int midi, int N, int seed)
{
    juce::Random rng (seed);
    const double ratios[7] = { 1.0, 2.0, 2.76, 5.4, 8.93, 11.34, 16.1 };
    const double amps[7]   = { 1.0, 0.5, 0.55, 0.3, 0.2, 0.12, 0.06 };
    const double taus[7]   = { 3.4, 2.6, 2.2, 1.3, 0.8, 0.5, 0.3 };
    const double tauScale  = juce::jlimit (0.35, 2.0, std::pow (2.0, -(midi - 60) / 36.0));

    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int k = 0; k < 7; ++k)
            addPartial (d, N, kGenRate, f * ratios[k] * (ch == 1 ? 1.0009 : 1.0), amps[k],
                        taus[k] * tauScale, rng.nextDouble() * kTwoPi);

        const int tick = juce::jmin (N, (int) (0.03 * kGenRate));
        for (int i = 0; i < tick; ++i)
            d[i] += 0.15f * (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-(double) i / (0.004 * kGenRate));
    }
}

// 4: cosmic saw (detuned band-limited saws, wide stereo)
static void genSaw (Buf& b, double f, int N, int seed)
{
    juce::Random rng (seed);
    const double cents[2][2] = { { -11.0, 3.0 }, { -3.0, 11.0 } };

    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int set = 0; set < 2; ++set)
        {
            const double fs = f * std::pow (2.0, cents[ch][set] / 1200.0);
            for (int n = 1; n <= 28; ++n)
            {
                const double freq = fs * n;
                if (freq > 0.42 * kGenRate) break;
                addPartial (d, N, kGenRate, freq, 1.0 / (double) n, 0.0, rng.nextDouble() * kTwoPi);
            }
        }
        addPartial (d, N, kGenRate, f * 0.5, 0.3, 0.0, 0.0);
    }
}

// 5: grain texture (shimmering partials + grainy noise, stereo)
static void genGrain (Buf& b, double f, int N, int seed)
{
    juce::Random rng (seed);
    for (int ch = 0; ch < 2; ++ch)
    {
        float* d = b.getWritePointer (ch);
        for (int k = 0; k < 16; ++k)
        {
            const double ratio = (double) (k + 1) + (k > 0 ? (rng.nextDouble() - 0.5) * 0.06 : 0.0);
            addPartial (d, N, kGenRate, f * ratio, 1.0 / std::pow ((double) (k + 1), 0.9), 0.0,
                        rng.nextDouble() * kTwoPi,
                        0.3 + rng.nextDouble() * 2.2, 0.95, rng.nextDouble() * kTwoPi);
        }
        const double gph = rng.nextDouble() * kTwoPi;
        float lp = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float nz = rng.nextFloat() * 2.0f - 1.0f;
            lp += 0.6f * (nz - lp);
            const float hp = nz - lp;   // brighter noise
            const float gate = 0.5f + 0.5f * (float) std::sin (kTwoPi * 3.1 * (double) i / kGenRate + gph);
            d[i] += 0.25f * hp * gate * gate;
        }
    }
}

// 6: sub (sine with a touch of harmonics, mono)
static void genSub (Buf& b, double f, int N)
{
    float* d = b.getWritePointer (0);
    addPartial (d, N, kGenRate, f, 1.0, 0.0, 0.0);
    addPartial (d, N, kGenRate, f * 2.0, 0.22, 0.0, 0.0);
    addPartial (d, N, kGenRate, f * 3.0, 0.07, 0.0, 0.0);
}

static std::unique_ptr<Instrument> buildInstrument (int which)
{
    struct Spec { int nch; double secs; bool loop; double lsSecs; double xfSecs; };
    static const Spec specs[7] =
    {
        { 1, 3.6, false, 0.0, 0.0 },  // rhodes
        { 2, 2.2, true,  0.8, 0.6 },  // dream pad
        { 2, 2.2, true,  0.8, 0.6 },  // air choir
        { 2, 4.0, false, 0.0, 0.0 },  // glass bell
        { 2, 2.2, true,  0.8, 0.6 },  // cosmic saw
        { 2, 2.2, true,  0.8, 0.6 },  // grain texture
        { 1, 1.6, true,  0.6, 0.4 },  // sub
    };
    const Spec sp = specs[which];

    auto inst = std::make_unique<Instrument>();

    for (int root = 24; root <= 100; root += 7)
    {
        inst->zones.emplace_back();
        SampleZone& z = inst->zones.back();

        const int N = (int) (sp.secs * kGenRate);
        z.data.setSize (sp.nch, N + 4);
        z.data.clear();
        z.rootNote = root;
        z.length = N;
        z.fileRate = kGenRate;

        const double f = juce::MidiMessage::getMidiNoteInHertz (root);
        const int seed = 1000 * (which + 1) + root;

        switch (which)
        {
            case 0: genRhodes (z.data, f, root, N);        break;
            case 1: genPad    (z.data, f, N, seed);        break;
            case 2: genChoir  (z.data, f, N, seed);        break;
            case 3: genBell   (z.data, f, root, N, seed);  break;
            case 4: genSaw    (z.data, f, N, seed);        break;
            case 5: genGrain  (z.data, f, N, seed);        break;
            default: genSub   (z.data, f, N);              break;
        }

        const int ls = (int) (sp.lsSecs * kGenRate);
        const int xf = (int) (sp.xfSecs * kGenRate);
        finishBuffer (z.data, N, sp.loop, ls, xf);
        if (sp.loop) { z.loopStart = ls; z.loopEnd = N; }
    }
    return inst;
}

static const std::vector<std::unique_ptr<Instrument>>& builtinBank()
{
    static const std::vector<std::unique_ptr<Instrument>> bank = []
    {
        std::vector<std::unique_ptr<Instrument>> v (7);
        std::vector<std::thread> threads;
        for (int i = 0; i < 7; ++i)
            threads.emplace_back ([&v, i] { v[(size_t) i] = buildInstrument (i); });
        for (auto& t : threads) t.join();
        return v;
    }();
    return bank;
}

static const char* builtinNames[7] = { "rhodes", "dream pad", "air choir", "glass bell",
                                       "cosmic saw", "grain texture", "sub" };

//------------------------------------------------------------------ user samples
// Note names: C3 = MIDI 60. Examples: "C3.wav", "F#2.wav", "pad_Bb1.wav", or "60.wav"
static int parseNoteName (const juce::String& s)
{
    if (s.isNotEmpty() && s.containsOnly ("0123456789"))
    {
        const int v = s.getIntValue();
        return v <= 127 ? v : -1;
    }

    static const std::regex re ("([A-Ga-g])([#b]?)(-?[0-9])");
    std::smatch m;
    const std::string str = s.toStdString();
    if (! std::regex_search (str, m, re))
        return -1;

    int semi = 0;
    switch (std::toupper (m[1].str()[0]))
    {
        case 'C': semi = 0;  break;
        case 'D': semi = 2;  break;
        case 'E': semi = 4;  break;
        case 'F': semi = 5;  break;
        case 'G': semi = 7;  break;
        case 'A': semi = 9;  break;
        default:  semi = 11; break;
    }
    if (m[2].str() == "#") ++semi;
    if (m[2].str() == "b") --semi;

    const int octave = std::stoi (m[3].str());
    const int midi = (octave + 2) * 12 + semi;
    return (midi >= 0 && midi <= 127) ? midi : -1;
}

static std::unique_ptr<Instrument> loadUserInstrument (const juce::File& dir, juce::AudioFormatManager& fm)
{
    auto inst = std::make_unique<Instrument>();
    inst->name = dir.getFileName();
    const bool looped = dir.getFileName().containsIgnoreCase ("loop");

    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav;*.aif;*.aiff;*.flac;*.ogg");

    for (auto& file : files)
    {
        const int note = parseNoteName (file.getFileNameWithoutExtension());
        if (note < 0)
            continue;

        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr)
            continue;

        const int len = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 30.0));
        if (len < 64)
            continue;

        const int nch = juce::jmin (2, (int) reader->numChannels);

        inst->zones.emplace_back();
        SampleZone& z = inst->zones.back();
        z.data.setSize (nch, len + 4);
        z.data.clear();
        reader->read (&z.data, 0, len, 0, true, nch > 1);
        z.length = len;
        z.fileRate = reader->sampleRate;
        z.rootNote = note;

        if (looped)
        {
            const int ls = (int) (len * 0.4);
            z.loopStart = ls;
            z.loopEnd = len;
            makeLoop (z.data, len, ls, juce::jmin (len / 8, ls));
        }
    }

    if (inst->zones.empty())
        return nullptr;
    return inst;
}

juce::File GenSynthAudioProcessor::getSampleFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("gen synth").getChildFile ("samples");
}

juce::StringArray GenSynthAudioProcessor::getSourceNames()
{
    juce::StringArray names;
    for (auto* n : builtinNames)
        names.add (n);

    juce::StringArray userNames;
    for (auto& d : getSampleFolder().findChildFiles (juce::File::findDirectories, false))
        userNames.add (d.getFileName());
    userNames.sort (true);

    for (int i = 0; i < juce::jmin (8, userNames.size()); ++i)
        names.add (userNames[i]);

    return names;
}

//==============================================================================
// Voice: two sample layers, per-layer pan / width, auto-pan, random + key pan
//==============================================================================
struct GenSound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

static inline float hermite (float x, float y0, float y1, float y2, float y3)
{
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + c0;
}

class GenVoice : public juce::SynthesiserVoice
{
public:
    GenVoice (juce::AudioProcessorValueTreeState& a,
              const std::vector<const Instrument*>& inst,
              std::atomic<float>& ln)
        : apvts (a), instruments (inst), lastNote (ln) {}

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<GenSound*> (s) != nullptr;
    }

    void setCurrentPlaybackSampleRate (double newRate) override
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);
        sr = newRate > 0.0 ? newRate : 44100.0;
        ampEnv.setSampleRate (sr);
        fltEnv.setSampleRate (sr);
        filt.prepare ({ sr, 512u, 2u });
        filt.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int pitchWheel) override
    {
        noteNum = midiNote;
        targetSemi = (float) midiNote;

        const float last = lastNote.load();
        curSemi = (param ("glide") > 0.5f && last >= 0.0f) ? last : targetSemi;
        lastNote.store (targetSemi);

        vel = velocity;
        pitchBend = ((float) pitchWheel - 8192.0f) / 8192.0f * 2.0f;
        apPhase = 0.0f;
        drift1 = rnd.nextFloat();
        drift2 = rnd.nextFloat();

        static const char* srcIds[2] = { "aSrc", "bSrc" };
        for (int l = 0; l < 2; ++l)
        {
            const int idx = (int) param (srcIds[l]);
            lay[l].inst = (idx >= 0 && idx < (int) instruments.size()) ? instruments[(size_t) idx] : nullptr;
            lay[l].zone = lay[l].inst != nullptr ? lay[l].inst->find (midiNote) : nullptr;
            lay[l].pos = 0.0;
            lay[l].active = lay[l].zone != nullptr;
            lay[l].rnd = rnd.nextFloat() * 2.0f - 1.0f;
        }

        updateEnvelopes();
        filt.reset();
        ampEnv.noteOn();
        fltEnv.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            ampEnv.noteOff();
            fltEnv.noteOff();
        }
        else
        {
            ampEnv.reset();
            fltEnv.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int v) override { pitchBend = ((float) v - 8192.0f) / 8192.0f * 2.0f; }
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! isVoiceActive())
            return;

        updateEnvelopes();

        static const char* ids[2][6] =
        {
            { "aSrc", "aLevel", "aPan", "aWidth", "aTune", "aFine" },
            { "bSrc", "bLevel", "bPan", "bWidth", "bTune", "bFine" }
        };
        float level[2], panBase[2], width[2], tune[2];
        for (int l = 0; l < 2; ++l)
        {
            level[l]   = param (ids[l][1]);
            panBase[l] = param (ids[l][2]);
            width[l]   = param (ids[l][3]);
            tune[l]    = param (ids[l][4]) + param (ids[l][5]) * 0.01f;
        }

        const float panRand  = param ("panRand");
        const float panKey   = param ("panKey");
        const float apRate   = param ("apRate");
        const float apDepth  = param ("apDepth");
        const float apOffB   = param ("apOffsetB");
        const float glideMs  = param ("glide");
        const float driftAmt = param ("drift");
        const float cutoff   = param ("cutoff");
        const float res      = param ("res");
        const float envAmt   = param ("fltEnvAmt");
        const float keyTrk   = param ("keyTrack");
        const float velFilt  = param ("velFilter");

        const float invSr = 1.0f / (float) sr;
        const float glideCoef = glideMs > 0.5f ? 1.0f - std::exp (-1.0f / (glideMs * 0.001f * (float) sr)) : 0.0f;
        const float keyT = juce::jlimit (-1.0f, 1.0f, ((float) noteNum - 60.0f) / 36.0f);
        const float cutMul = std::exp2 (keyTrk * ((float) noteNum - 60.0f) / 12.0f)
                           * std::exp2 (velFilt * (vel - 0.5f) * 3.0f);

        filt.setResonance (0.7f + res * res * 12.0f);

        const float twoPi = juce::MathConstants<float>::twoPi;
        const float quarterPi = juce::MathConstants<float>::pi * 0.25f;

        for (int i = 0; i < num; ++i)
        {
            // --- pitch (glide in semitones, drift in cents)
            if (glideCoef > 0.0f) curSemi += (targetSemi - curSemi) * glideCoef;
            else                  curSemi = targetSemi;

            const float driftCents = driftAmt * (0.6f * std::sin (twoPi * drift1) + 0.4f * std::sin (twoPi * drift2));
            drift1 += 0.23f * invSr; if (drift1 >= 1.0f) drift1 -= 1.0f;
            drift2 += 0.57f * invSr; if (drift2 >= 1.0f) drift2 -= 1.0f;
            const float semis = curSemi + driftCents * 0.01f + pitchBend;

            apPhase += apRate * invSr; if (apPhase >= 1.0f) apPhase -= 1.0f;

            float vL = 0.0f, vR = 0.0f;
            bool alive = false;

            for (int l = 0; l < 2; ++l)
            {
                Layer& ly = lay[l];
                if (! ly.active || level[l] <= 0.0001f)
                    continue;
                alive = true;

                const SampleZone& z = *ly.zone;
                const float ratio = std::exp2 ((semis + tune[l] - (float) z.rootNote) / 12.0f)
                                  * (float) (z.fileRate / sr);

                const int i1 = (int) ly.pos;
                const float fr = (float) (ly.pos - (double) i1);
                const int i0 = juce::jmax (0, i1 - 1);
                const float* dl = z.data.getReadPointer (0);
                const float* dr = z.data.getNumChannels() > 1 ? z.data.getReadPointer (1) : dl;

                float sL = hermite (fr, dl[i0], dl[i1], dl[i1 + 1], dl[i1 + 2]);
                float sR = hermite (fr, dr[i0], dr[i1], dr[i1 + 1], dr[i1 + 2]);

                ly.pos += (double) ratio;
                if (z.loopEnd > 0)
                {
                    while (ly.pos >= (double) z.loopEnd)
                        ly.pos -= (double) (z.loopEnd - z.loopStart);
                }
                else if (ly.pos >= (double) (z.length - 2))
                {
                    ly.active = false;
                }

                // stereo width (mid/side)
                const float m = 0.5f * (sL + sR);
                const float s = 0.5f * (sL - sR) * width[l];
                sL = m + s;
                sR = m - s;

                // pan = layer pan + random per note + key tracking + auto-pan
                const float lfo = std::sin (twoPi * (apPhase + (l == 1 ? apOffB : 0.0f)));
                const float pan = juce::jlimit (-1.0f, 1.0f,
                                                panBase[l] + panRand * ly.rnd + panKey * keyT + apDepth * lfo);
                const float th = (pan + 1.0f) * quarterPi;
                const float gL = std::cos (th) * 1.4142f;
                const float gR = std::sin (th) * 1.4142f;

                vL += sL * gL * level[l];
                vR += sR * gR * level[l];
            }

            if (! alive)
            {
                clearCurrentNote();
                break;
            }

            // --- filter
            const float fe = fltEnv.getNextSample();
            if ((i & 7) == 0)
            {
                const float fc = juce::jlimit (20.0f, 0.45f * (float) sr, cutoff * cutMul * std::exp2 (envAmt * fe));
                filt.setCutoffFrequency (fc);
            }
            const float l = filt.processSample (0, vL);
            const float r = filt.processSample (1, vR);

            const float a = ampEnv.getNextSample() * vel * 0.35f;

            out.addSample (0, start + i, l * a);
            if (out.getNumChannels() > 1)
                out.addSample (1, start + i, r * a);

            if (! ampEnv.isActive())
            {
                clearCurrentNote();
                break;
            }
        }
    }

private:
    struct Layer
    {
        const Instrument* inst = nullptr;
        const SampleZone* zone = nullptr;
        double pos = 0.0;
        bool active = false;
        float rnd = 0.0f;
    };

    float param (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    void updateEnvelopes()
    {
        ampEnv.setParameters ({ param ("ampA"), param ("ampD"), param ("ampS"), param ("ampR") });
        fltEnv.setParameters ({ param ("fltA"), param ("fltD"), param ("fltS"), param ("ampR") });
    }

    juce::AudioProcessorValueTreeState& apvts;
    const std::vector<const Instrument*>& instruments;
    std::atomic<float>& lastNote;

    Layer lay[2];
    double sr = 44100.0;
    int noteNum = 60;
    float curSemi = 60.0f, targetSemi = 60.0f, vel = 1.0f, pitchBend = 0.0f;
    float apPhase = 0.0f, drift1 = 0.0f, drift2 = 0.0f;

    juce::ADSR ampEnv, fltEnv;
    juce::dsp::StateVariableTPTFilter<float> filt;
    juce::Random rnd;
};

//==============================================================================
// Parameters (grouped; all lowercase)
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout GenSynthAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    std::unique_ptr<juce::AudioProcessorParameterGroup> grp;

    auto G = [&] (const juce::String& id, const juce::String& name)
    {
        grp = std::make_unique<juce::AudioProcessorParameterGroup> (id, name, "|");
    };
    auto endG = [&] { layout.add (std::move (grp)); };

    auto F = [&] (const juce::String& id, const juce::String& name, float lo, float hi, float def, float skew = 1.0f)
    {
        grp->addChild (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name,
            juce::NormalisableRange<float> (lo, hi, 0.0f, skew), def));
    };
    auto I = [&] (const juce::String& id, const juce::String& name, int lo, int hi, int def)
    {
        grp->addChild (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { id, 1 }, name, lo, hi, def));
    };
    auto C = [&] (const juce::String& id, const juce::String& name, const juce::StringArray& choices, int def)
    {
        grp->addChild (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, def));
    };

    const auto sources = getSourceNames();

    auto layerParams = [&] (const juce::String& p, const juce::String& title, int defSrc, float defLevel)
    {
        G ("layer" + p, title);
        C (p + "Src",   "source", sources, defSrc);
        F (p + "Level", "level", 0.0f, 1.0f, defLevel);
        F (p + "Pan",   "pan", -1.0f, 1.0f, 0.0f);
        F (p + "Width", "stereo width", 0.0f, 1.5f, 1.0f);
        I (p + "Tune",  "tune (semitones)", -24, 24, 0);
        F (p + "Fine",  "fine tune (cents)", -50.0f, 50.0f, 0.0f);
        endG();
    };

    layerParams ("a", "layer a", 0, 0.9f);
    layerParams ("b", "layer b", 1, 0.0f);

    G ("panning", "panning");
    F ("panRand",   "random pan per note", 0.0f, 1.0f, 0.0f);
    F ("panKey",    "key pan (low left, high right)", -1.0f, 1.0f, 0.0f);
    F ("apRate",    "auto-pan rate (hz)", 0.05f, 8.0f, 0.5f, 0.4f);
    F ("apDepth",   "auto-pan depth", 0.0f, 1.0f, 0.0f);
    F ("apOffsetB", "auto-pan layer b offset", 0.0f, 1.0f, 0.5f);
    endG();

    G ("pitch", "pitch");
    F ("glide", "glide (ms)", 0.0f, 1000.0f, 0.0f, 0.5f);
    F ("drift", "pitch drift (cents)", 0.0f, 50.0f, 4.0f);
    endG();

    G ("filter", "filter");
    F ("cutoff",    "cutoff", 20.0f, 20000.0f, 9000.0f, 0.3f);
    F ("res",       "resonance", 0.0f, 1.0f, 0.05f);
    F ("fltEnvAmt", "env amount (oct)", -4.0f, 6.0f, 0.0f);
    F ("fltA",      "env attack", 0.001f, 5.0f, 0.01f, 0.35f);
    F ("fltD",      "env decay", 0.001f, 8.0f, 0.5f, 0.35f);
    F ("fltS",      "env sustain", 0.0f, 1.0f, 1.0f);
    F ("keyTrack",  "key tracking", 0.0f, 1.0f, 0.3f);
    F ("velFilter", "velocity to cutoff", 0.0f, 1.0f, 0.3f);
    endG();

    G ("amp", "amp");
    F ("ampA", "attack", 0.001f, 5.0f, 0.003f, 0.35f);
    F ("ampD", "decay", 0.001f, 8.0f, 0.5f, 0.35f);
    F ("ampS", "sustain", 0.0f, 1.0f, 0.85f);
    F ("ampR", "release", 0.001f, 8.0f, 0.45f, 0.35f);
    endG();

    G ("fx", "effects");
    F ("drive",     "drive", 0.0f, 1.0f, 0.05f);
    F ("chorusMix", "chorus", 0.0f, 1.0f, 0.2f);
    F ("delayMix",  "delay mix", 0.0f, 1.0f, 0.0f);
    F ("delayTime", "delay time (ms)", 50.0f, 1000.0f, 375.0f, 0.6f);
    F ("delayFb",   "delay feedback", 0.0f, 0.9f, 0.35f);
    F ("revMix",    "reverb mix", 0.0f, 1.0f, 0.22f);
    F ("revSize",   "reverb size", 0.0f, 1.0f, 0.8f);
    F ("revDamp",   "reverb darkness", 0.0f, 1.0f, 0.5f);
    F ("master",    "master (db)", -24.0f, 6.0f, -3.0f);
    endG();

    return layout;
}

//==============================================================================
// Preset bank
// sources: 0 rhodes, 1 dream pad, 2 air choir, 3 glass bell, 4 cosmic saw, 5 grain texture, 6 sub
//==============================================================================
struct Preset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values;
};

static const std::vector<Preset>& presets()
{
    static const std::vector<Preset> p =
    {
        { "init", {} },

        // ---- keys: robert glasper style
        { "glasper rhodes", {
            { "aSrc", 0 }, { "aLevel", 0.9f }, { "aPan", -0.15f },
            { "bSrc", 3 }, { "bLevel", 0.12f }, { "bTune", 12 }, { "bPan", 0.3f },
            { "panKey", 0.35f }, { "apDepth", 0.15f }, { "apRate", 4.8f },
            { "cutoff", 7000 }, { "fltEnvAmt", 0.5f }, { "fltD", 0.8f }, { "fltS", 0.7f },
            { "ampR", 0.5f }, { "drive", 0.12f }, { "chorusMix", 0.25f },
            { "revMix", 0.2f }, { "revSize", 0.6f }, { "revDamp", 0.55f } } },

        { "black radio wurli", {
            { "aSrc", 0 }, { "aLevel", 0.85f }, { "bSrc", 0 }, { "bLevel", 0.35f }, { "bTune", -12 },
            { "cutoff", 4500 }, { "res", 0.15f }, { "drive", 0.35f }, { "chorusMix", 0.1f },
            { "apDepth", 0.4f }, { "apRate", 5.5f }, { "apOffsetB", 0.5f },
            { "revMix", 0.12f }, { "revSize", 0.5f } } },

        { "late night keys", {
            { "aSrc", 0 }, { "aLevel", 0.85f }, { "bSrc", 1 }, { "bLevel", 0.35f }, { "bWidth", 1.3f },
            { "cutoff", 5500 }, { "ampA", 0.005f }, { "ampR", 0.9f },
            { "delayMix", 0.15f }, { "delayTime", 450 }, { "delayFb", 0.35f },
            { "revMix", 0.35f }, { "revSize", 0.8f }, { "revDamp", 0.7f } } },

        // ---- dreamy r&b: jhene aiko style
        { "sail out pad", {
            { "aSrc", 1 }, { "aLevel", 0.7f }, { "aWidth", 1.4f },
            { "bSrc", 2 }, { "bLevel", 0.5f }, { "bFine", 7 },
            { "cutoff", 3200 }, { "fltEnvAmt", 1.2f }, { "fltA", 0.8f }, { "fltD", 1.5f }, { "fltS", 0.6f },
            { "ampA", 0.6f }, { "ampD", 1.0f }, { "ampS", 0.85f }, { "ampR", 2.2f },
            { "drift", 10 }, { "panRand", 0.2f }, { "apDepth", 0.2f }, { "apRate", 0.15f },
            { "chorusMix", 0.35f }, { "delayMix", 0.22f }, { "delayTime", 500 }, { "delayFb", 0.4f },
            { "revMix", 0.5f }, { "revSize", 0.9f }, { "revDamp", 0.6f } } },

        { "aiko air", {
            { "aSrc", 2 }, { "aLevel", 0.8f }, { "bSrc", 3 }, { "bLevel", 0.25f }, { "bTune", 12 },
            { "cutoff", 4500 }, { "ampA", 0.25f }, { "ampR", 1.6f }, { "drift", 8 },
            { "apDepth", 0.3f }, { "apRate", 0.2f },
            { "chorusMix", 0.3f }, { "delayMix", 0.2f }, { "delayTime", 375 },
            { "revMix", 0.5f }, { "revSize", 0.9f } } },

        { "lotus keys", {
            { "aSrc", 0 }, { "aLevel", 0.8f }, { "bSrc", 2 }, { "bLevel", 0.45f }, { "bTune", 12 }, { "bFine", 5 },
            { "cutoff", 6000 }, { "ampR", 1.0f }, { "panKey", 0.3f },
            { "chorusMix", 0.3f }, { "delayMix", 0.15f }, { "revMix", 0.45f }, { "revSize", 0.85f } } },

        { "moon bath", {
            { "aSrc", 1 }, { "aLevel", 0.8f }, { "bSrc", 5 }, { "bLevel", 0.25f },
            { "cutoff", 1500 }, { "res", 0.12f }, { "fltEnvAmt", 1.5f }, { "fltA", 1.2f }, { "fltD", 2.0f }, { "fltS", 0.5f },
            { "ampA", 1.0f }, { "ampR", 2.8f }, { "drift", 14 }, { "apDepth", 0.35f }, { "apRate", 0.09f },
            { "chorusMix", 0.4f }, { "revMix", 0.6f }, { "revSize", 0.95f }, { "revDamp", 0.8f } } },

        // ---- ethereal / dark: whitearmour style
        { "white armour bell", {
            { "aSrc", 3 }, { "aLevel", 0.8f }, { "bSrc", 1 }, { "bLevel", 0.35f }, { "bTune", -12 },
            { "cutoff", 2800 }, { "ampA", 0.005f }, { "ampD", 1.5f }, { "ampS", 0.5f }, { "ampR", 2.0f },
            { "panRand", 0.6f }, { "panKey", 0.4f }, { "drive", 0.15f },
            { "delayMix", 0.3f }, { "delayTime", 420 }, { "delayFb", 0.45f },
            { "revMix", 0.6f }, { "revSize", 0.92f }, { "revDamp", 0.75f } } },

        { "frozen choir", {
            { "aSrc", 2 }, { "aLevel", 0.7f }, { "bSrc", 3 }, { "bLevel", 0.4f }, { "bTune", 12 },
            { "cutoff", 5000 }, { "ampA", 0.4f }, { "ampR", 2.5f },
            { "apDepth", 0.3f }, { "apRate", 0.3f }, { "chorusMix", 0.45f },
            { "revMix", 0.65f }, { "revSize", 0.95f }, { "revDamp", 0.55f } } },

        { "glass cathedral", {
            { "aSrc", 3 }, { "aLevel", 0.7f }, { "aTune", -12 }, { "bSrc", 2 }, { "bLevel", 0.45f },
            { "cutoff", 3500 }, { "ampA", 0.15f }, { "ampR", 3.5f }, { "drive", 0.1f },
            { "panRand", 0.5f }, { "panKey", 0.5f }, { "delayMix", 0.25f },
            { "revMix", 0.75f }, { "revSize", 1.0f }, { "revDamp", 0.7f } } },

        // ---- cosmic analog: mike dean style
        { "cosmic lead", {
            { "aSrc", 4 }, { "aLevel", 0.75f }, { "bSrc", 4 }, { "bLevel", 0.45f }, { "bTune", 12 }, { "bFine", 8 },
            { "glide", 90 }, { "drift", 18 },
            { "cutoff", 3800 }, { "res", 0.25f }, { "fltEnvAmt", 1.4f }, { "fltD", 0.5f }, { "fltS", 0.6f },
            { "ampA", 0.01f }, { "ampS", 0.85f }, { "ampR", 0.5f },
            { "drive", 0.3f }, { "chorusMix", 0.4f }, { "panRand", 0.15f },
            { "delayMix", 0.3f }, { "delayTime", 375 }, { "delayFb", 0.45f }, { "revMix", 0.4f } } },

        { "dean sub", {
            { "aSrc", 6 }, { "aLevel", 0.9f }, { "bSrc", 4 }, { "bLevel", 0.25f }, { "bTune", -12 },
            { "glide", 110 }, { "drift", 0 }, { "cutoff", 700 }, { "drive", 0.5f },
            { "ampA", 0.005f }, { "ampS", 1.0f }, { "ampR", 0.2f },
            { "chorusMix", 0.0f }, { "revMix", 0.05f } } },

        { "mike dean pad", {
            { "aSrc", 4 }, { "aLevel", 0.6f }, { "aWidth", 1.5f }, { "bSrc", 1 }, { "bLevel", 0.5f },
            { "cutoff", 2400 }, { "res", 0.2f }, { "fltEnvAmt", 1.8f }, { "fltA", 0.9f }, { "fltD", 1.8f }, { "fltS", 0.5f },
            { "ampA", 0.7f }, { "ampR", 2.5f }, { "drift", 15 }, { "apDepth", 0.15f }, { "apRate", 0.12f },
            { "drive", 0.2f }, { "chorusMix", 0.45f },
            { "delayMix", 0.3f }, { "delayTime", 480 }, { "revMix", 0.55f }, { "revSize", 0.92f }, { "revDamp", 0.7f } } },

        // ---- generative textures: micah frank style
        { "shimmer field", {
            { "aSrc", 5 }, { "aLevel", 0.8f }, { "aWidth", 1.5f }, { "bSrc", 3 }, { "bLevel", 0.3f }, { "bTune", 12 },
            { "ampA", 0.5f }, { "ampR", 2.5f }, { "cutoff", 7000 }, { "drift", 6 },
            { "apDepth", 0.45f }, { "apRate", 0.35f }, { "apOffsetB", 0.5f }, { "panRand", 0.7f },
            { "delayMix", 0.3f }, { "delayTime", 333 }, { "delayFb", 0.5f },
            { "revMix", 0.55f }, { "revSize", 0.9f } } },

        { "granular rain", {
            { "aSrc", 5 }, { "aLevel", 0.85f }, { "aTune", 12 },
            { "bSrc", 5 }, { "bLevel", 0.5f }, { "bTune", 19 }, { "bFine", 6 },
            { "ampA", 0.02f }, { "ampD", 0.8f }, { "ampS", 0.3f }, { "ampR", 1.2f },
            { "fltEnvAmt", 0.5f }, { "panRand", 1.0f }, { "panKey", 0.6f },
            { "apDepth", 0.5f }, { "apRate", 1.8f },
            { "delayMix", 0.35f }, { "delayTime", 250 }, { "delayFb", 0.55f }, { "revMix", 0.5f } } },

        { "modular drift", {
            { "aSrc", 5 }, { "aLevel", 0.6f }, { "bSrc", 1 }, { "bLevel", 0.45f },
            { "glide", 200 }, { "drift", 30 },
            { "cutoff", 1800 }, { "res", 0.3f }, { "fltEnvAmt", 2.0f }, { "fltA", 1.5f }, { "fltD", 3.0f }, { "fltS", 0.4f },
            { "ampA", 1.2f }, { "ampR", 3.0f }, { "apDepth", 0.6f }, { "apRate", 0.07f },
            { "delayMix", 0.3f }, { "revMix", 0.6f } } },

        { "autopan cloud", {
            { "aSrc", 5 }, { "aLevel", 0.7f }, { "bSrc", 2 }, { "bLevel", 0.5f },
            { "apDepth", 0.9f }, { "apRate", 0.25f }, { "apOffsetB", 0.5f },
            { "cutoff", 4500 }, { "ampA", 0.6f }, { "ampR", 2.2f },
            { "chorusMix", 0.4f }, { "revMix", 0.55f } } },
    };
    return p;
}

//==============================================================================
// Processor
//==============================================================================
GenSynthAudioProcessor::GenSynthAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    getSampleFolder().createDirectory();

    // built-in synthesized multisamples (built once, shared)
    for (auto& inst : builtinBank())
        instruments.push_back (inst.get());

    // user sample folders (same order as getSourceNames())
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    juce::StringArray userNames;
    for (auto& d : getSampleFolder().findChildFiles (juce::File::findDirectories, false))
        userNames.add (d.getFileName());
    userNames.sort (true);

    for (int i = 0; i < juce::jmin (8, userNames.size()); ++i)
    {
        auto inst = loadUserInstrument (getSampleFolder().getChildFile (userNames[i]), fm);
        instruments.push_back (inst.get());       // nullptr = silent source
        if (inst != nullptr)
            userInstruments.push_back (std::move (inst));
    }

    for (int i = 0; i < 8; ++i)
        synth.addVoice (new GenVoice (apvts, instruments, lastNote));

    synth.addSound (new GenSound());
}

bool GenSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void GenSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    synth.setCurrentPlaybackSampleRate (sampleRate);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2u };
    chorus.prepare (spec);
    chorus.setRate (0.35f);
    chorus.setDepth (0.5f);
    chorus.setCentreDelay (18.0f);
    chorus.setFeedback (0.0f);

    reverb.setSampleRate (sampleRate);
    reverb.reset();

    delayBuf.setSize (2, (int) (2.0 * sampleRate) + 4);
    delayBuf.clear();
    delayWrite = 0;
    delaySmoothed = 0.375f * (float) sampleRate;
    fbLpL = fbLpR = 0.0f;
}

void GenSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    buffer.clear();
    synth.renderNextBlock (buffer, midi, 0, n);

    if (buffer.getNumChannels() < 2)
        return;

    auto P = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    // --- drive (soft saturation)
    const float drive = P ("drive");
    if (drive > 0.001f)
    {
        const float g = 1.0f + drive * 8.0f;
        const float comp = 1.0f / std::sqrt (g);
        for (int i = 0; i < n; ++i)
        {
            L[i] = std::tanh (g * L[i]) * comp;
            R[i] = std::tanh (g * R[i]) * comp;
        }
    }

    // --- chorus
    chorus.setMix (P ("chorusMix"));
    {
        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        chorus.process (ctx);
    }

    // --- dark ping-pong delay
    const float dMix = P ("delayMix");
    if (dMix > 0.001f)
    {
        const float targetD = juce::jmax (2.0f, P ("delayTime") * 0.001f * (float) currentSampleRate);
        const float fb = juce::jmin (0.92f, P ("delayFb"));
        const int size = delayBuf.getNumSamples();
        float* c0 = delayBuf.getWritePointer (0);
        float* c1 = delayBuf.getWritePointer (1);

        for (int i = 0; i < n; ++i)
        {
            delaySmoothed += (targetD - delaySmoothed) * 0.0005f;
            float rp = (float) delayWrite - delaySmoothed;
            while (rp < 0.0f) rp += (float) size;
            const int i0 = (int) rp;
            const int i1 = (i0 + 1) % size;
            const float fr = rp - (float) i0;

            const float dl = c0[i0] * (1.0f - fr) + c0[i1] * fr;
            const float dr = c1[i0] * (1.0f - fr) + c1[i1] * fr;

            fbLpL += 0.3f * (dl - fbLpL);
            fbLpR += 0.3f * (dr - fbLpR);

            c0[delayWrite] = L[i] + fbLpR * fb;
            c1[delayWrite] = R[i] + fbLpL * fb;

            L[i] += dl * dMix;
            R[i] += dr * dMix;

            delayWrite = (delayWrite + 1) % size;
        }
    }

    // --- dark reverb
    {
        const float mix = P ("revMix");
        juce::Reverb::Parameters rp;
        rp.roomSize   = P ("revSize");
        rp.damping    = P ("revDamp");
        rp.wetLevel   = mix * 0.5f;
        rp.dryLevel   = 1.0f - 0.4f * mix;
        rp.width      = 1.0f;
        rp.freezeMode = 0.0f;
        reverb.setParameters (rp);
        reverb.processStereo (L, R, n);
    }

    // --- master + safety clip
    const float gain = juce::Decibels::decibelsToGain (P ("master"));
    for (int i = 0; i < n; ++i)
    {
        L[i] = juce::jlimit (-1.0f, 1.0f, L[i] * gain);
        R[i] = juce::jlimit (-1.0f, 1.0f, R[i] * gain);
    }
}

//==============================================================================
juce::AudioProcessorEditor* GenSynthAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

int GenSynthAudioProcessor::getNumPrograms() { return (int) presets().size(); }

const juce::String GenSynthAudioProcessor::getProgramName (int index)
{
    if (index >= 0 && index < getNumPrograms())
        return presets()[(size_t) index].name;
    return {};
}

void GenSynthAudioProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= getNumPrograms())
        return;

    currentProgram = index;

    for (auto* p : getParameters())
        p->setValueNotifyingHost (p->getDefaultValue());

    for (auto& kv : presets()[(size_t) index].values)
        if (auto* p = apvts.getParameter (kv.first))
            p->setValueNotifyingHost (p->convertTo0to1 (kv.second));
}

void GenSynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void GenSynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GenSynthAudioProcessor();
}
