#pragma once
// Shared sample-kit loading for forsitan: a minimal WAV reader, the
// plugin-wide "kits folder" setting, and the helpers that turn that folder
// into kits of samples. pellicula introduced all of this and vates reuses it,
// so the two modules browse one library of kits.
//
// Everything lives in the forsitan_sampler namespace and every function is
// inline: each src/*.cpp is a separate translation unit linked into one
// plugin, so a file-scope type or symbol shared by two of them is an ODR
// violation (see check_symbols.py).

#include <rack.hpp>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace forsitan_sampler {

using namespace rack;

// ── a loaded sample ───────────────────────────────────────────────────────────
// `l` is the left channel, or the whole thing when mono; `r` is empty unless
// the file was stereo and the caller asked to keep it.
struct Sample {
	std::vector<float> l;
	std::vector<float> r;
	float sampleRate = 44100.f;

	bool   stereo() const { return !r.empty(); }
	size_t frames() const { return l.size(); }
	bool   empty()  const { return l.size() < 2; }
	// channel c of frame i, mono files reading the same data on both sides
	float  at(int c, size_t i) const { return (c && stereo()) ? r[i] : l[i]; }
};

// A kit: the samples of one subfolder, ordered by filename.
struct Kit {
	std::string name;
	std::vector<Sample> samples;
	std::vector<std::string> sampleNames;   // file stems, index-aligned
};

// ── minimal WAV loader ────────────────────────────────────────────────────────
// Canonical RIFF/WAVE PCM: 8/16/24/32-bit int and 32-bit float, any channel
// count. Little-endian hosts only (all Rack targets). With `mono` the
// channels are averaged into `l`; otherwise a stereo file keeps both and
// anything wider is folded down to two.

inline uint32_t rd32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rd16(const uint8_t* p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

inline float pcmSample(const uint8_t* s, int bits, bool isFloat) {
	if (isFloat && bits == 32) {
		uint32_t u = rd32(s);
		float fv;
		std::memcpy(&fv, &u, 4);
		return fv;
	}
	if (bits == 16)
		return (int16_t)rd16(s) / 32768.f;
	if (bits == 24) {
		int32_t u = (s[0]) | (s[1] << 8) | (s[2] << 16);
		if (u & 0x800000)
			u |= ~0xFFFFFF;
		return u / 8388608.f;
	}
	if (bits == 32)
		return (int32_t)rd32(s) / 2147483648.f;
	if (bits == 8)
		return ((int)s[0] - 128) / 128.f;   // 8-bit WAV is unsigned
	return 0.f;
}

inline bool loadWav(const std::string& path, Sample& out, bool mono = true) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (len < 44) {
		std::fclose(f);
		return false;
	}
	std::vector<uint8_t> buf(len);
	size_t got = std::fread(buf.data(), 1, len, f);
	std::fclose(f);
	if ((long)got != len)
		return false;
	if (std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data() + 8, "WAVE", 4))
		return false;

	int fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 44100;
	const uint8_t* pcm = nullptr;
	uint32_t pcmBytes = 0;

	size_t pos = 12;
	while (pos + 8 <= (size_t)len) {
		const uint8_t* ch = buf.data() + pos;
		uint32_t size = rd32(ch + 4);
		const uint8_t* body = ch + 8;
		if (pos + 8 + size > (size_t)len)
			size = (uint32_t)((size_t)len - pos - 8);
		if (!std::memcmp(ch, "fmt ", 4) && size >= 16) {
			fmt      = rd16(body);
			channels = rd16(body + 2);
			rate     = rd32(body + 4);
			bits     = rd16(body + 14);
			if (fmt == 0xFFFE && size >= 40)   // WAVE_FORMAT_EXTENSIBLE
				fmt = rd16(body + 24);
		}
		else if (!std::memcmp(ch, "data", 4)) {
			pcm = body;
			pcmBytes = size;
		}
		pos += 8 + size + (size & 1);
	}
	if (!pcm || channels < 1 || bits < 8)
		return false;

	// anything that is not IEEE float is read as PCM ints, as pellicula
	// always has: rejecting unknown tags here would refuse files that used
	// to load
	const bool isFloat = (fmt == 3);
	const int  bytesPS = bits / 8;
	if (bytesPS == 0)
		return false;
	const uint32_t frames = pcmBytes / (bytesPS * channels);
	out.sampleRate = (float)rate;
	out.l.assign(frames, 0.f);
	out.r.clear();
	const bool keepStereo = !mono && channels >= 2;
	if (keepStereo)
		out.r.assign(frames, 0.f);

	for (uint32_t i = 0; i < frames; ++i) {
		if (keepStereo) {
			// left and right straight through; extra channels fold into both
			float accL = 0.f, accR = 0.f;
			int nL = 0, nR = 0;
			for (int c = 0; c < channels; ++c) {
				float v = pcmSample(pcm + (size_t)(i * channels + c) * bytesPS, bits, isFloat);
				if (c % 2 == 0) { accL += v; nL++; }
				else            { accR += v; nR++; }
			}
			out.l[i] = accL / std::max(nL, 1);
			out.r[i] = accR / std::max(nR, 1);
		}
		else {
			float acc = 0.f;
			for (int c = 0; c < channels; ++c)
				acc += pcmSample(pcm + (size_t)(i * channels + c) * bytesPS, bits, isFloat);
			out.l[i] = acc / channels;
		}
	}
	return true;
}

// ── the plugin-wide "kits folder" setting ─────────────────────────────────────
// One folder, shared by every module and every instance. The file keeps
// pellicula's name because pellicula wrote it first and users' settings must
// keep working.

namespace detail {
struct KitsFolderState {
	std::mutex mutex;
	std::string folder;
	bool loaded = false;
};
inline KitsFolderState& kitsFolderState() {
	static KitsFolderState s;
	return s;
}
}   // namespace detail

inline std::string settingsPath() {
	return asset::user("pellicula/settings.json");
}

inline void loadSettingsOnce() {
	detail::KitsFolderState& s = detail::kitsFolderState();
	std::lock_guard<std::mutex> lock(s.mutex);
	if (s.loaded)
		return;
	s.loaded = true;
	FILE* f = std::fopen(settingsPath().c_str(), "r");
	if (!f)
		return;
	json_error_t err;
	json_t* root = json_loadf(f, 0, &err);
	std::fclose(f);
	if (!root)
		return;
	if (json_t* kf = json_object_get(root, "kitsFolder"))
		s.folder = json_string_value(kf);
	json_decref(root);
}

inline std::string getKitsFolder() {
	detail::KitsFolderState& s = detail::kitsFolderState();
	std::lock_guard<std::mutex> lock(s.mutex);
	return s.folder;
}

// `persist` is false only for tests, which must not rewrite the user's
// settings file to point at a temporary folder.
inline void setKitsFolder(const std::string& path, bool persist = true) {
	detail::KitsFolderState& s = detail::kitsFolderState();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.folder = path;
	s.loaded = true;
	if (!persist)
		return;
	system::createDirectories(system::getDirectory(settingsPath()));
	json_t* root = json_object();
	json_object_set_new(root, "kitsFolder", json_string(path.c_str()));
	if (FILE* f = std::fopen(settingsPath().c_str(), "w")) {
		json_dumpf(root, f, JSON_INDENT(2));
		std::fclose(f);
	}
	json_decref(root);
}

// ── browsing the folder ───────────────────────────────────────────────────────

// natural (numeric-aware, case-insensitive) ordering: 1, 2, 10 not 1, 10, 2
inline bool naturalLess(const std::string& A, const std::string& B) {
	std::string a = string::lowercase(A), b = string::lowercase(B);
	size_t i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
			size_t i0 = i, j0 = j;
			while (i < a.size() && std::isdigit((unsigned char)a[i])) ++i;
			while (j < b.size() && std::isdigit((unsigned char)b[j])) ++j;
			std::string na = a.substr(i0, i - i0), nb = b.substr(j0, j - j0);
			size_t pa = na.find_first_not_of('0'), pb = nb.find_first_not_of('0');
			na = (pa == std::string::npos) ? "" : na.substr(pa);
			nb = (pb == std::string::npos) ? "" : nb.substr(pb);
			if (na.size() != nb.size()) return na.size() < nb.size();
			if (na != nb)               return na < nb;
		}
		else {
			if (a[i] != b[j]) return a[i] < b[j];
			++i; ++j;
		}
	}
	return a.size() < b.size();
}

// the immediate subfolders of the kits folder - each one is a kit
inline std::vector<std::string> listKits(const std::string& folder) {
	std::vector<std::string> kits;
	if (folder.empty() || !system::isDirectory(folder))
		return kits;
	for (const std::string& e : system::getEntries(folder))
		if (system::isDirectory(e))
			kits.push_back(system::getFilename(e));
	std::sort(kits.begin(), kits.end(), naturalLess);
	return kits;
}

// the .wav files of one kit folder, in natural filename order
inline std::vector<std::string> listKitFiles(const std::string& dir, int maxFiles) {
	std::vector<std::string> files;
	if (dir.empty() || !system::isDirectory(dir))
		return files;
	for (const std::string& e : system::getEntries(dir))
		if (system::isFile(e) && string::lowercase(system::getExtension(e)) == ".wav")
			files.push_back(e);
	std::sort(files.begin(), files.end(), [](const std::string& x, const std::string& y) {
		return naturalLess(system::getFilename(x), system::getFilename(y));
	});
	if (maxFiles > 0 && (int)files.size() > maxFiles)
		files.resize(maxFiles);
	return files;
}

}   // namespace forsitan_sampler
