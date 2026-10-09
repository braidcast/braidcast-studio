#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_RING_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_RING_HPP_

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace Voice {

// Single-producer / single-consumer float ring. The producer is the libobs audio
// thread, so Write allocates nothing, takes no lock and never blocks: a full ring
// drops the newest samples and counts them (dropping the oldest instead would need
// the reader's index, which the writer must not move). The buffer is allocated once,
// in the constructor, on whatever thread builds the capture.
//
// One producer and one consumer only. Reset is for the consumer side while the
// producer is known to be stopped (the capture is unbound between segments).
class SpscRing {
public:
	explicit SpscRing(size_t capacity) : buf_(capacity + 1) {}

	size_t Capacity() const { return buf_.size() - 1; }

	// Producer. Returns how many samples were stored; the rest were dropped.
	size_t Write(const float *src, size_t n)
	{
		const size_t cap = buf_.size();
		const size_t w = write_.load(std::memory_order_relaxed);
		const size_t r = read_.load(std::memory_order_acquire);
		const size_t freeSlots = (r + cap - w - 1) % cap;
		const size_t take = n < freeSlots ? n : freeSlots;
		if (take > 0) {
			const size_t first = take < cap - w ? take : cap - w;
			std::memcpy(buf_.data() + w, src, first * sizeof(float));
			if (take > first) {
				std::memcpy(buf_.data(), src + first, (take - first) * sizeof(float));
			}
			write_.store((w + take) % cap, std::memory_order_release);
		}
		if (take < n) {
			dropped_.fetch_add(n - take, std::memory_order_relaxed);
		}
		return take;
	}

	// Consumer. Returns how many samples were copied out (up to `n`).
	size_t Read(float *dst, size_t n)
	{
		const size_t cap = buf_.size();
		const size_t r = read_.load(std::memory_order_relaxed);
		const size_t w = write_.load(std::memory_order_acquire);
		const size_t have = (w + cap - r) % cap;
		const size_t take = n < have ? n : have;
		if (take > 0) {
			const size_t first = take < cap - r ? take : cap - r;
			std::memcpy(dst, buf_.data() + r, first * sizeof(float));
			if (take > first) {
				std::memcpy(dst + first, buf_.data(), (take - first) * sizeof(float));
			}
			read_.store((r + take) % cap, std::memory_order_release);
		}
		return take;
	}

	// Consumer.
	size_t Available() const
	{
		const size_t cap = buf_.size();
		const size_t r = read_.load(std::memory_order_relaxed);
		const size_t w = write_.load(std::memory_order_acquire);
		return (w + cap - r) % cap;
	}

	size_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

	// Consumer, with the producer stopped.
	void Reset()
	{
		read_.store(0, std::memory_order_relaxed);
		write_.store(0, std::memory_order_relaxed);
		dropped_.store(0, std::memory_order_relaxed);
	}

private:
	// The audio thread may not take a lock, and a non-lock-free atomic is one in disguise.
	static_assert(std::atomic<size_t>::is_always_lock_free, "SpscRing needs lock-free size_t atomics");

	std::vector<float> buf_; // one slot is always empty, to tell full from empty
	std::atomic<size_t> read_{0};
	std::atomic<size_t> write_{0};
	std::atomic<size_t> dropped_{0};
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_RING_HPP_
