#include "silent_audio.h"
#include <array>
#include <cstdio>

template <typename T> bool check() {
    std::array<T, 66> left, right;
    left.fill(T{0.75});
    right.fill(T{-0.5});
    T *channels[] = {left.data() + 1, right.data() + 1};
    clap_audio_buffer_t buffer{};
    buffer.channel_count = 2;
    if constexpr (sizeof(T) == sizeof(float)) buffer.data32 = channels;
    else buffer.data64 = channels;
    clap_process_t process{};
    process.frames_count = 64;
    process.audio_outputs = &buffer;
    process.audio_outputs_count = 1;
    lumipaint::clearSilentOutput(&process);
    for (unsigned i = 1; i <= 64; ++i)
        if (left[i] != 0 || right[i] != 0) return false;
    return buffer.constant_mask == 3 && left.front() == T{0.75} &&
        left.back() == T{0.75} && right.front() == T{-0.5} && right.back() == T{-0.5};
}

int main() {
    clap_process_t empty{};
    lumipaint::clearSilentOutput(&empty);
    bool passed = check<float>() && check<double>();
    std::puts(passed ? "PASS: silent float/double buffers and untouched guards" : "FAIL");
    return passed ? 0 : 1;
}
