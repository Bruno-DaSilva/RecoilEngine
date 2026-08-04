// Glibc-floor smoke test, compiled per target arch at image-bake time.
// Exercises exactly what killed the gcc+sysroot approach: <thread>/<mutex>
// (glibc-2.30 pthread calls in new-glibc libstdc++ headers) and the static
// C++ runtime archives (arc4random & friends), plus C++23 library features.
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <format>
#include <ranges>
#include <vector>
#include <cstdio>

int main() {
	std::mutex m;
	std::condition_variable cv;
	bool ready = false;
	std::thread t([&] {
		std::lock_guard<std::mutex> lk(m);
		ready = true;
		cv.notify_one();
	});
	{
		std::unique_lock<std::mutex> lk(m);
		cv.wait_for(lk, std::chrono::seconds(5), [&] { return ready; });
	}
	t.join();

	std::vector<int> v{1, 2, 3};
	long sum = 0;
	for (int x : v | std::views::transform([](int i) { return i * 2; }))
		sum += x;

	auto msg = std::format("threads+mutex+cv+ranges ok, sum={}", sum);
	puts(msg.c_str());
	return sum == 12 ? 0 : 1;
}
