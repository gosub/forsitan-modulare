// vates_probe — measure and audition the generated banks.
//
//   vates_probe kits            every family of every ported pages64 kit
//   vates_probe tonal           the imber-derived tonal families
//   vates_probe bank <seed>     the six banks vates builds from one seed
//   vates_probe wav <dir>       write every sample of bank 0..5 as a WAV
//
// No Rack: the generators are header-only C++11.
#include "../src/vates/kits.hpp"
#include "../src/vates/bank.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

using namespace vates_bank;

static void stats(const std::vector<float>& L, const std::vector<float>& R,
                  float sr, float& durS, float& peak, float& rms, float& centroid,
                  float& width) {
	size_t n = L.size();
	durS = n / sr;
	peak = 0.f;
	double sum = 0.0, sumDiff = 0.0;
	for (size_t i = 0; i < n; i++) {
		float m = 0.5f * (L[i] + R[i]);
		float d = 0.5f * (L[i] - R[i]);
		peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
		sum += (double)m * m;
		sumDiff += (double)d * d;
	}
	rms = n ? (float)std::sqrt(sum / n) : 0.f;
	width = sum > 1e-12 ? (float)std::sqrt(sumDiff / std::max(sum, 1e-12)) : 0.f;
	// spectral centroid by zero-crossing proxy of the mono mix (cheap, robust)
	int zc = 0;
	float prev = 0.f;
	for (size_t i = 0; i < n; i++) {
		float m = 0.5f * (L[i] + R[i]);
		if ((m > 0.f) != (prev > 0.f))
			zc++;
		prev = m;
	}
	centroid = n ? 0.5f * zc * sr / n : 0.f;
}

static void report(const char* label, const std::vector<float>& L,
                   const std::vector<float>& R, float sr) {
	float d, p, r, c, w;
	stats(L, R, sr, d, p, r, c, w);
	printf("  %-22s %7.3f s  peak %5.3f  rms %6.4f  zcr %7.1f Hz  width %4.2f%s\n",
	       label, d, p, r, c, w, (p < 0.05f ? "   <-- QUIET" : ""));
}

int main(int argc, char** argv) {
	const float sr = 44100.f;
	std::string mode = argc > 1 ? argv[1] : "kits";

	if (mode == "kits") {
		for (int kit = 0; kit < vates_kits::KIT_COUNT; kit++) {
			static const char* kitNames[4] = {"drums", "objects", "grains", "micro"};
			printf("%s\n", kitNames[kit]);
			for (int fam = 0; fam < vates_kits::familyCount(kit); fam++) {
				imber_dsp::Rng rng;
				rng.seed(0x51ede5eeull + kit * 977 + fam);
				std::vector<float> L, R;
				vates_kits::render(kit, fam, 0.5f, rng, sr, L, R);
				report(vates_kits::familyName(kit, fam), L, R, sr);
			}
		}
		return 0;
	}

	if (mode == "tonal") {
		for (int fam = 0; fam < tonalFamilyCount(); fam++) {
			imber_dsp::Rng rng;
			rng.seed(0x70e5ull + fam);
			std::vector<float> L, R;
			renderTonal(fam, rng, sr, L, R);
			report(tonalFamilyName(fam), L, R, sr);
		}
		return 0;
	}

	if (mode == "bank") {
		uint64_t seed = argc > 2 ? strtoull(argv[2], nullptr, 0) : 1;
		BankSet set;
		build(set, seed, sr, nullptr, nullptr);
		for (int b = 0; b < kNumBanks; b++) {
			printf("%s\n", set.banks[b].name.c_str());
			for (int s = 0; s < kSamplesPerBank; s++)
				report(set.banks[b].sampleNames[s].c_str(),
				       set.banks[b].L[s], set.banks[b].R[s], sr);
		}
		return 0;
	}

	if (mode == "wav") {
		std::string dir = argc > 2 ? argv[2] : ".";
		uint64_t seed = argc > 3 ? strtoull(argv[3], nullptr, 0) : 1;
		BankSet set;
		build(set, seed, sr, nullptr, nullptr);
		for (int b = 0; b < kNumBanks; b++)
			for (int s = 0; s < kSamplesPerBank; s++) {
				char path[512];
				snprintf(path, sizeof path, "%s/%d_%s_%d_%s.wav", dir.c_str(),
				         b, set.banks[b].name.c_str(), s,
				         set.banks[b].sampleNames[s].c_str());
				const std::vector<float>& L = set.banks[b].L[s];
				const std::vector<float>& R = set.banks[b].R[s];
				FILE* f = fopen(path, "wb");
				if (!f) { printf("cannot write %s\n", path); continue; }
				uint32_t n = (uint32_t)L.size(), bytes = n * 4;
				auto w32 = [&](uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f);
				                             fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); };
				auto w16 = [&](uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); };
				fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVE", 1, 4, f);
				fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2); w32((uint32_t)sr);
				w32((uint32_t)sr * 4); w16(4); w16(16);
				fwrite("data", 1, 4, f); w32(bytes);
				for (uint32_t i = 0; i < n; i++) {
					w16((uint16_t)(int16_t)lrintf(std::max(-1.f, std::min(1.f, L[i])) * 32767.f));
					w16((uint16_t)(int16_t)lrintf(std::max(-1.f, std::min(1.f, R[i])) * 32767.f));
				}
				fclose(f);
			}
		printf("wrote %d banks to %s\n", kNumBanks, dir.c_str());
		return 0;
	}

	printf("usage: vates_probe [kits|tonal|bank <seed>|wav <dir> [seed]]\n");
	return 2;
}
