/**
 * Long file name tests. Most need MFS_LONG_FILE_NAMES; a few check behaviour that
 * matters either way (long name entries are removed with their file, long names
 * are rejected when support is compiled out).
 */
#include "TestEnv.h"

#include "net/mdawson/filesystem/LongName.h"

using namespace Mfs;

/** Write a hand made long name entry (one entry, up to 13 ASCII characters). */
static void addRawLongNameEntry(DirectoryListing &dl, const char *longName, const char shortName[11]) {
	DirectoryEntryLong entry;
	memset(&entry, 0, sizeof(entry));
	uint16_t chars[13];
	uint32_t length = strlen(longName);
	for (uint32_t i = 0; i < 13; i++) {
		chars[i] = i < length ? (uint16_t)longName[i] : (i == length ? 0 : 0xFFFF);
	}
	entry.LDIR_Ord = 1 | LFN_LAST_ENTRY;
	memcpy(entry.LDIR_Name1, chars, 10);
	memcpy(entry.LDIR_Name2, chars + 5, 12);
	memcpy(entry.LDIR_Name3, chars + 11, 4);
	entry.LDIR_Attr = ATTR_LONG_NAME;
	entry.LDIR_Chksum = Lfn::shortNameChecksum((const uint8_t*)shortName);
	dl.addEntry(*(const DirectoryEntry*)&entry);
}

static void test_checksum() {
	CHECK_EQ(Lfn::shortNameChecksum((const uint8_t*)"README  TXT"), 115);
	CHECK_EQ(Lfn::shortNameChecksum((const uint8_t*)"MYLONG~1TXT"), 112);
}

static void test_remove_deletes_long_name_entries() {
	// Applies with and without long name support: a file created elsewhere with a
	// long name must not leave orphaned long name entries when deleted.
	Volume v;
	CHECK_OK();
	{
		DirectoryListing dl(*v.fat, "/");
		addRawLongNameEntry(dl, "Hello.txt", "HELLO   TXT");
		DirectoryEntry entry;
		memset(&entry, 0, sizeof(entry));
		memcpy(entry.DIR_Name, "HELLO   TXT", 11);
		entry.DIR_Attr = ATTR_ARCHIVE;
		dl.addEntry(entry);
		CHECK_OK();
	}
	CHECK_EQ(countUsedSlots(*v.fat, "/"), 2);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
	{
		DirectoryListing dl(*v.fat, "/");
		CHECK(dl.next());
		CHECK_EQ(dl.longNameEntryCount(), 1);
		char name[32];
		dl.name(name, sizeof(name));
#ifdef MFS_LONG_FILE_NAMES
		CHECK(dl.hasLongName());
		CHECK(strcmp(name, "Hello.txt") == 0);
#else
		CHECK(!dl.hasLongName());
		CHECK(strcmp(name, "HELLO.TXT") == 0);
#endif
	}
	File file(*v.fat, "/", "HELLO.TXT");
	CHECK_OK();
	file.remove();
	CHECK_OK();
	CHECK_EQ(countUsedSlots(*v.fat, "/"), 0);
}

static void test_orphaned_long_name_entries_are_ignored() {
	Volume v;
	CHECK_OK();
	{
		DirectoryListing dl(*v.fat, "/");
		// Checksum belongs to a different short name.
		addRawLongNameEntry(dl, "Wrong.txt", "OTHER   TXT");
		DirectoryEntry entry;
		memset(&entry, 0, sizeof(entry));
		memcpy(entry.DIR_Name, "REAL    TXT", 11);
		entry.DIR_Attr = ATTR_ARCHIVE;
		dl.addEntry(entry);
		CHECK_OK();
	}
	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.next());
	CHECK(!dl.hasLongName());
	CHECK_EQ(dl.longNameEntryCount(), 0);
	char name[32];
	dl.name(name, sizeof(name));
	CHECK(strcmp(name, "REAL.TXT") == 0);
	CHECK(!dl.next());
	File wrong(*v.fat, "/", "Wrong.txt");
#ifdef MFS_LONG_FILE_NAMES
	CHECK_EQ(mdError, File_fileNotFound);
#else
	CHECK(mdError != 0);
#endif
}

#ifndef MFS_LONG_FILE_NAMES

static void test_long_names_rejected_without_support() {
	Volume v;
	CHECK_OK();
	File file(*v.fat, "/", "My Long File Name.txt", true);
	CHECK_EQ(mdError, File_invalidName);
	makeDirectory(*v.fat, "/", "My Directory");
	CHECK_EQ(mdError, File_invalidName);
	CHECK_EQ(countUsedSlots(*v.fat, "/"), 0);
	// Lower case 8.3 names are simply stored upper case.
	writeFile(*v.fat, "/", "readme.txt", 5, 1, 5);
	CHECK_OK();
	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.find("README.TXT"));
	CHECK_EQ(countUsedSlots(*v.fat, "/"), 1);
}

#else

static bool shortNameIs(DirectoryListing &dl, const char shortName[12]) {
	return memcmp(dl.directoryEntry()->DIR_Name, shortName, 11) == 0;
}

static void test_basis_names() {
	char out[11];
	bool lossy;
	uint16_t name[64];
	struct Case { const char *longName; const char *basis; bool lossy; };
	static const Case cases[] = {
		{ "readme.txt",          "README  TXT", false },
		{ "My Long File.txt",    "MYLONGFITXT", true },
		{ ".bashrc",             "BASHRC     ", true },
		{ "a+b.c",               "A_B     C  ", true },
		{ "archive.tar.gz",      "ARCHIVETGZ ", true },
		{ "caf\xC3\xA9.doc",     "CAF_    DOC", true },
		{ "noext",               "NOEXT      ", false },
		{ "x.html",              "X       HTM", true },
		{ "...",                 "_          ", true },
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int32_t length = Lfn::fromUtf8(cases[i].longName, strlen(cases[i].longName), name);
		if (strcmp(cases[i].longName, "...") == 0) {
			CHECK_EQ(length, -1); // nothing left after removing trailing periods
			continue;
		}
		CHECK(length > 0);
		Lfn::makeBasisName(name, (uint32_t)length, out, &lossy);
		if (memcmp(out, cases[i].basis, 11) != 0 || lossy != cases[i].lossy) {
			printf("    basis of \"%s\" was \"%.11s\" lossy=%d\n", cases[i].longName, out, lossy);
			CHECK(false);
		}
	}

	memcpy(out, "ARCHIVETGZ ", 11);
	char tailed[11];
	Lfn::applyNumericTail(out, 1, tailed);
	CHECK(memcmp(tailed, "ARCHIV~1GZ ", 11) == 0);
	Lfn::applyNumericTail(out, 42, tailed);
	CHECK(memcmp(tailed, "ARCHI~42GZ ", 11) == 0);
	Lfn::applyNumericTail(out, 123456, tailed);
	CHECK(memcmp(tailed, "A~123456GZ ", 11) == 0);
	Lfn::applyNumericTail("AB      TXT", 3, tailed);
	CHECK(memcmp(tailed, "AB~3    TXT", 11) == 0);
}

static void test_utf8_conversion() {
	uint16_t name[LFN_MAX_LENGTH];
	// 2, 3 and 4 byte sequences; the emoji needs a surrogate pair.
	const char *text = "caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x93\x81";
	int32_t length = Lfn::fromUtf8(text, strlen(text), name);
	CHECK_EQ(length, 9);
	CHECK_EQ(name[3], 0xE9);
	CHECK_EQ(name[5], 0x2615);
	CHECK_EQ(name[7], 0xD83D);
	CHECK_EQ(name[8], 0xDCC1);
	char back[32];
	CHECK_EQ(Lfn::toUtf8(name, (uint32_t)length, back, sizeof(back)), strlen(text));
	CHECK(strcmp(back, text) == 0);

	// Truncation never splits a character.
	CHECK_EQ(Lfn::toUtf8(name, (uint32_t)length, back, 6), 5); // "café" + terminator fits exactly
	CHECK(strcmp(back, "caf\xC3\xA9") == 0);
	CHECK_EQ(Lfn::toUtf8(name, (uint32_t)length, back, 5), 3); // é would not fit with the terminator
	CHECK(strcmp(back, "caf") == 0);

	// Invalid UTF-8 and characters not allowed in names.
	static const char *invalid[] = {
		"\xC0\xAF", "\xE2\x82", "\xED\xA0\x80", "\xF8\x88\x80\x80\x80", "\x80",
		"a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\\b", "a\x01" "b", "", " ", "..",
	};
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		if (Lfn::fromUtf8(invalid[i], strlen(invalid[i]), name) != -1) {
			printf("    accepted invalid name #%u\n", i);
			CHECK(false);
		}
	}
	// Trailing periods and spaces are dropped.
	CHECK_EQ(Lfn::fromUtf8("name. . ", 8, name), 4);
	CHECK(Lfn::equalsIgnoreCase(name, 4, name, 4));
}

static void test_create_and_open_long_name() {
	Volume v;
	CHECK_OK();
	writeFile(*v.fat, "/", "My Long File Name.txt", 1000, 3, 300);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", "My Long File Name.txt", 1000, 3, 300));
	CHECK(verifyFile(*v.fat, "/", "MY LONG FILE NAME.TXT", 1000, 3, 300)); // case insensitive
	CHECK(verifyFile(*v.fat, "/", "MYLONG~1.TXT", 1000, 3, 300));          // by short alias

	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.next());
	CHECK(dl.hasLongName());
	CHECK(shortNameIs(dl, "MYLONG~1TXT"));
	CHECK_EQ(dl.longNameEntryCount(), 2); // 21 characters = 2 entries of 13
	char name[64];
	CHECK_EQ(dl.name(name, sizeof(name)), 21);
	CHECK(strcmp(name, "My Long File Name.txt") == 0);
	CHECK(!dl.next());
	CHECK_EQ(countUsedSlots(*v.fat, "/"), 3);

	// Creating with a differently cased name opens the existing file.
	File again(*v.fat, "/", "my long file name.TXT", true);
	CHECK_OK();
	CHECK_EQ(again.length(), 1000);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
}

static void test_alias_numbering() {
	Volume v;
	CHECK_OK();
	char longName[40], expected[12];
	for (int i = 1; i <= 11; i++) {
		snprintf(longName, sizeof(longName), "Long File Name %d.txt", i);
		writeFile(*v.fat, "/", longName, (uint32_t)i, (uint32_t)i, 64);
		CHECK_OK();
	}
	for (int i = 1; i <= 11; i++) {
		snprintf(longName, sizeof(longName), "Long File Name %d.txt", i);
		DirectoryListing dl(*v.fat, "/");
		CHECK(dl.find(longName));
		if (i < 10) snprintf(expected, sizeof(expected), "LONGFI~%dTXT", i);
		else snprintf(expected, sizeof(expected), "LONGF~%dTXT", i);
		if (!shortNameIs(dl, expected)) {
			printf("    %s got alias %.11s, expected %s\n", longName, (const char*)dl.directoryEntry()->DIR_Name, expected);
			CHECK(false);
		}
		CHECK(verifyFile(*v.fat, "/", longName, (uint32_t)i, (uint32_t)i, 64));
	}
}

static void test_lower_case_short_name() {
	Volume v;
	CHECK_OK();
	writeFile(*v.fat, "/", "readme.txt", 10, 1, 10);
	CHECK_OK();
	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.next());
	CHECK(shortNameIs(dl, "README  TXT")); // no numeric tail needed
	CHECK(dl.hasLongName());
	char name[32];
	dl.name(name, sizeof(name));
	CHECK(strcmp(name, "readme.txt") == 0);
	// Upper case names need no long name entries.
	writeFile(*v.fat, "/", "PLAIN.TXT", 10, 1, 10);
	CHECK_EQ(countUsedSlots(*v.fat, "/"), 3);
	// Both names refer to the same file.
	File same(*v.fat, "/", "README.TXT", true);
	CHECK_OK();
	CHECK_EQ(same.length(), 10);
	CHECK_EQ(countEntries(*v.fat, "/"), 2);
}

static void test_unicode_names() {
	Volume v;
	CHECK_OK();
	const char *dirName = "\xF0\x9F\x93\x81 Fotos";     // "📁 Fotos"
	const char *fileName = "caf\xC3\xA9 \xE2\x98\x95.txt"; // "café ☕.txt"
	makeDirectory(*v.fat, "/", dirName);
	CHECK_OK();
	char path[64];
	snprintf(path, sizeof(path), "/%s", dirName);
	writeFile(*v.fat, path, fileName, 77, 9, 77);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, path, fileName, 77, 9, 77));

	DirectoryListing dl(*v.fat, path);
	CHECK_OK();
	char name[64];
	bool found = false;
	while (dl.next()) {
		dl.name(name, sizeof(name));
		if (strcmp(name, fileName) == 0) found = true;
	}
	CHECK(found);
}

static void test_name_lengths() {
	Volume v;
	CHECK_OK();
	char name[300];
	memset(name, 'n', 255);
	name[255] = 0;
	writeFile(*v.fat, "/", name, 5, 1, 5); // the longest allowed: 20 entries
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", name, 5, 1, 5));
	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.find(name));
	CHECK_EQ(dl.longNameEntryCount(), 20);
	char back[300];
	CHECK_EQ(dl.name(back, sizeof(back)), 255);
	CHECK(strcmp(back, name) == 0);

	memset(name, 'n', 256);
	name[256] = 0;
	File tooLong(*v.fat, "/", name, true);
	CHECK_EQ(mdError, File_invalidName);

	// Trailing periods and spaces are ignored.
	writeFile(*v.fat, "/", "trailing. . ", 3, 2, 3);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", "trailing", 3, 2, 3));
}

static void test_invalid_long_names() {
	Volume v;
	CHECK_OK();
	static const char *invalid[] = { "a:b.txt", "what?.txt", "star*.txt", "pipe|.txt", "quote\".txt",
	                                 "less<.txt", "back\\slash.txt", ".", "..", "", "   " };
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		File file(*v.fat, "/", invalid[i], true);
		if (mdError != File_invalidName) {
			printf("    \"%s\": error %lu\n", invalid[i], (unsigned long)mdError);
			CHECK(false);
		}
	}
	CHECK_EQ(countUsedSlots(*v.fat, "/"), 0);
	// Characters valid in long names but not in short ones are fine.
	writeFile(*v.fat, "/", "a+b=c [1], d;e.txt", 4, 1, 4);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", "a+b=c [1], d;e.txt", 4, 1, 4));
}

static void test_runs_span_clusters_and_remove() {
	Volume v;
	CHECK_OK();
	// 43 character names take 4 long entries + 1 short = 5 slots; with 16 slots per
	// 512 byte cluster, most runs straddle a cluster boundary.
	const int count = 13;
	char name[64];
	for (int i = 0; i < count; i++) {
		snprintf(name, sizeof(name), "A rather long file name with number %02d.data", i);
		writeFile(*v.fat, "/", name, 100 + (uint32_t)i, (uint32_t)i, 64);
		CHECK_OK();
	}
	CHECK_EQ(countUsedSlots(*v.fat, "/"), count * 5);
	CHECK_EQ(countEntries(*v.fat, "/"), count);
	for (int i = 0; i < count; i++) {
		snprintf(name, sizeof(name), "A rather long file name with number %02d.data", i);
		CHECK(verifyFile(*v.fat, "/", name, 100 + (uint32_t)i, (uint32_t)i, 64));
	}
	// Remove every other file: all their entries must go.
	for (int i = 0; i < count; i += 2) {
		snprintf(name, sizeof(name), "A rather long file name with number %02d.data", i);
		File file(*v.fat, "/", name);
		file.remove();
		CHECK_OK();
	}
	int remaining = count / 2;
	CHECK_EQ(countUsedSlots(*v.fat, "/"), remaining * 5);
	CHECK_EQ(countEntries(*v.fat, "/"), remaining);
	for (int i = 1; i < count; i += 2) {
		snprintf(name, sizeof(name), "A rather long file name with number %02d.data", i);
		CHECK(verifyFile(*v.fat, "/", name, 100 + (uint32_t)i, (uint32_t)i, 64));
	}
	// New names reuse the freed runs instead of growing the directory.
	uint32_t freeBefore = v.fat->countFreeClusters();
	for (int i = 0; i < count; i += 2) {
		snprintf(name, sizeof(name), "Replacement file with a long name %02d", i);
		File file(*v.fat, "/", name, true);
		CHECK_OK();
	}
	CHECK_EQ(v.fat->countFreeClusters(), freeBefore);
	CHECK_EQ(countEntries(*v.fat, "/"), count);
}

static void test_long_directory_paths() {
	Volume v;
	CHECK_OK();
	makeDirectory(*v.fat, "/", "My Projects");
	CHECK_OK();
	makeDirectory(*v.fat, "/My Projects", "Second Level Directory");
	CHECK_OK();
	makeDirectory(*v.fat, "/", "my projects");
	CHECK_EQ(mdError, File_alreadyExists);
	writeFile(*v.fat, "/My Projects/Second Level Directory", "Read Me First.txt", 2000, 5, 700);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/my projects/SECOND LEVEL DIRECTORY", "read me first.txt", 2000, 5, 700));
	CHECK(verifyFile(*v.fat, "/MYPROJ~1/SECOND~1", "README~1.TXT", 2000, 5, 700));
	CHECK(verifyFile(*v.fat, "/My Projects/Second Level Directory/../Second Level Directory",
	                 "Read Me First.txt", 2000, 5, 700));
	DirectoryListing missing(*v.fat, "/My Projects/Nope");
	CHECK_EQ(mdError, DirectoryListing_directoryNotFound);
}

#endif

const TestFramework::TestCase LONG_NAME_TESTS[] = {
	{ "lfn_checksum", test_checksum },
	{ "lfn_remove_deletes_long_name_entries", test_remove_deletes_long_name_entries },
	{ "lfn_orphaned_entries_are_ignored", test_orphaned_long_name_entries_are_ignored },
#ifndef MFS_LONG_FILE_NAMES
	{ "lfn_rejected_without_support", test_long_names_rejected_without_support },
#else
	{ "lfn_basis_names", test_basis_names },
	{ "lfn_utf8_conversion", test_utf8_conversion },
	{ "lfn_create_and_open", test_create_and_open_long_name },
	{ "lfn_alias_numbering", test_alias_numbering },
	{ "lfn_lower_case_short_name", test_lower_case_short_name },
	{ "lfn_unicode_names", test_unicode_names },
	{ "lfn_name_lengths", test_name_lengths },
	{ "lfn_invalid_names", test_invalid_long_names },
	{ "lfn_runs_span_clusters_and_remove", test_runs_span_clusters_and_remove },
	{ "lfn_long_directory_paths", test_long_directory_paths },
#endif
};

const int LONG_NAME_TEST_COUNT = (int)(sizeof(LONG_NAME_TESTS) / sizeof(LONG_NAME_TESTS[0]));
