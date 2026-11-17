#ifndef net_mdawson_test_TestFramework
#define net_mdawson_test_TestFramework

/**
 * A tiny test framework that only needs printf, so the same tests run on a
 * Linux host and on a microcontroller (output over UART/USB serial).
 */

#include <stdio.h>
#include <string.h>

namespace TestFramework {
	typedef void (*TestFunction)();

	struct TestCase {
		const char *name;
		TestFunction function;
	};

	/** Set when the current test fails; checked by the runner. */
	extern bool currentTestFailed;
	extern bool currentTestSkipped;

	inline void fail(const char *file, int line, const char *message) {
		printf("    %s:%d: %s\n", file, line, message);
		currentTestFailed = true;
	}

	/**
	 * Run tests whose "pass/name" contains filter (or all if filter is null/empty).
	 * @return the number of failed tests.
	 */
	int run(const TestCase *tests, int count, const char *filter, const char *pass);

	/** Mark the current test as skipped (e.g. needs more RAM than available). */
	void skip(const char *reason);
}

#define CHECK(cond) do { \
	if (!(cond)) { TestFramework::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed"); return; } \
} while (0)

#define CHECK_EQ(actual, expected) do { \
	unsigned long _a = (unsigned long)(actual), _e = (unsigned long)(expected); \
	if (_a != _e) { \
		char _msg[192]; \
		snprintf(_msg, sizeof(_msg), "CHECK_EQ(%s, %s) failed: got %lu (0x%lx), expected %lu (0x%lx)", \
			#actual, #expected, _a, _a, _e, _e); \
		TestFramework::fail(__FILE__, __LINE__, _msg); return; \
	} \
} while (0)

/** Check that the driver's global error is clear. */
#define CHECK_OK() CHECK_EQ(mdError, 0)

#endif
