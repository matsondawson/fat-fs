#include "TestFramework.h"

namespace TestFramework {
	bool currentTestFailed = false;
	bool currentTestSkipped = false;

	void skip(const char *reason) {
		printf("    skipped: %s\n", reason);
		currentTestSkipped = true;
	}

	int run(const TestCase *tests, int count, const char *filter, const char *pass) {
		int failed = 0, ran = 0, skipped = 0;
		char fullName[128];
		for (int i = 0; i < count; i++) {
			snprintf(fullName, sizeof(fullName), "%s/%s", pass, tests[i].name);
			if (filter && *filter && !strstr(fullName, filter)) continue;
			printf("[ RUN  ] %s\n", fullName);
			currentTestFailed = false;
			currentTestSkipped = false;
			tests[i].function();
			ran++;
			if (currentTestFailed) {
				failed++;
				printf("[ FAIL ] %s\n", fullName);
			} else if (currentTestSkipped) {
				skipped++;
				printf("[ SKIP ] %s\n", fullName);
			} else {
				printf("[  OK  ] %s\n", fullName);
			}
		}
		if (ran) {
			printf("%s: %d tests, %d passed, %d skipped, %d failed\n\n", pass, ran, ran - failed - skipped, skipped, failed);
		}
		return failed;
	}
}
