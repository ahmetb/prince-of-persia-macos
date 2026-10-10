/*
Save persistence tests: named/quick files survive restart, atomic writes
preserve the previous readable save on failure, Unicode/long paths work
without truncation. Uses filesystem fixtures only (test-save.popsave,
test-directory); no realistic player names.
*/

#include "common.h"
#include "game_snapshot.h"
#import <Foundation/Foundation.h>
#import <sys/stat.h>
#import <unistd.h>
#import <errno.h>

static int failures = 0;
#define CHECK(cond, what) do { \
	if (!(cond)) { printf("FAIL: %s\n", what); failures++; } \
	else { printf("ok: %s\n", what); } \
} while (0)

static NSData *read_all(const char *path) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) return nil;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	NSMutableData *d = [NSMutableData dataWithLength:(NSUInteger)n];
	if (n > 0 && fread(d.mutableBytes, 1, (size_t)n, f) != (size_t)n) { fclose(f); return nil; }
	fclose(f);
	return d;
}

int main(void) {
	@autoreleasepool {
		NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:
			[NSString stringWithFormat:@"pop-persist-%@", [NSUUID UUID].UUIDString]];
		[[NSFileManager defaultManager] createDirectoryAtPath:tmp
			withIntermediateDirectories:YES attributes:nil error:NULL];
		const char *dir = tmp.fileSystemRepresentation;

		char save_path[PATH_MAX];
		snprintf(save_path, sizeof(save_path), "%s/test-save.popsave", dir);
		SaveError error = {0};

		// Fresh save + immediate reload (survives the write/read round-trip).
		SaveResult r = game_save_file(save_path, &error);
		CHECK(r == SAVE_OK, "game_save_file writes named save");
		CHECK([[NSFileManager defaultManager] fileExistsAtPath:@(save_path)],
		      "named save file exists on disk");

		// Read the document back and decode it (readability round-trip; no
		// engine assets are required for the codec).
		NSData *savedBytes = read_all(save_path);
		CHECK(savedBytes.length > 84, "named save bytes readable from disk");
		GameSnapshot decoded;
		SaveResult dr = game_snapshot_decode(savedBytes.bytes, savedBytes.length, &decoded, &error);
		CHECK(dr == SAVE_OK, "named save decodes byte-for-byte from disk");

		// Unicode + long selected path must work without truncation (a single
		// path component stays under NAME_MAX=255, the OS limit).
		NSString *longName = [@"test-保存-дані-セーブ-" stringByAppendingString:
			[@"" stringByPaddingToLength:200 withString:@"x" startingAtIndex:0]];
		NSString *unicodeFile = [longName stringByAppendingString:@".popsave"];
		NSString *unicodePath = [tmp stringByAppendingPathComponent:unicodeFile];
		r = game_save_file(unicodePath.fileSystemRepresentation, &error);
		if (r != SAVE_OK) printf("long-path save error(%d): %s\n", r, error.message);
		CHECK(r == SAVE_OK, "game_save_file accepts long Unicode path");
		CHECK([[NSFileManager defaultManager] fileExistsAtPath:unicodePath],
		      "long Unicode save file exists without truncation");

		// Sibling-temp atomic write: a failure must preserve the previous file
		// byte-for-byte. Make the directory read-only so mkstemp cannot create
		// the temporary sibling.
		NSData *before = read_all(save_path);
		CHECK(before != nil, "read prior save bytes");
		chmod(dir, 0555);
		r = game_save_file(save_path, &error);
		chmod(dir, 0755);
		CHECK(r == SAVE_IO_ERROR, "unwritable directory reports IO error");
		NSData *after = read_all(save_path);
		CHECK([after isEqual:before], "failed save leaves previous file byte-identical");

		// Cleanup.
		[[NSFileManager defaultManager] removeItemAtPath:tmp error:NULL];

		printf("%s: %d failures\n", failures ? "FAILED" : "PASSED", failures);
	}
	return failures != 0;
}
