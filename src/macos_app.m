/*
Prince of Persia — native macOS app support layer.
Copyright (C) 2026  The Prince of Persia macOS port authors.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#import "macos_app.h"
#import "game_commands.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <SDL.h>
#import <CommonCrypto/CommonDigest.h>
#import <sys/stat.h>
#import <sys/param.h>
#import <errno.h>
#import <stdint.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

// Engine globals (defined in src/data.c).
extern int g_argc;
extern char **g_argv;

#define POP_SUPPORT_DIR_NAME "local.princeofpersia.metal"

#define POP_ARCHIVE_URL @"https://archive.org/download/PERSIA_VGA/PERSIA.zip"
#define POP_ARCHIVE_BYTES ((size_t)353211)
// Verified during planning (SHA-256 of the exact PERSIA.zip payload).
static const char pop_archive_sha256_hex[] =
	"99973555d3a2db46d0a7be1dcf17f4834e85d0b4d0bca2adb6bad48ac333db40";

static const char *const pop_dat_names[] = {
	"PRINCE.DAT", "LEVELS.DAT", "KID.DAT", "TITLE.DAT", "PV.DAT",
	"VDUNGEON.DAT", "VPALACE.DAT",
	"GUARD.DAT", "GUARD1.DAT", "GUARD2.DAT", "FAT.DAT", "SKEL.DAT",
	"VIZIER.DAT", "SHADOW.DAT",
	"IBM_SND1.DAT", "IBM_SND2.DAT", "DIGISND1.DAT", "DIGISND2.DAT",
	"DIGISND3.DAT", "MIDISND1.DAT", "MIDISND2.DAT",
};
#define POP_DAT_COUNT ((int)(sizeof(pop_dat_names) / sizeof(pop_dat_names[0])))

// ---------------------------------------------------------------------------
// Cached storage roots
// ---------------------------------------------------------------------------
static char s_support_root[PATH_MAX];
static char s_game_data[PATH_MAX];
static char s_resources[PATH_MAX];
static char s_settings[PATH_MAX];
static char s_saves[PATH_MAX];
static char s_replays[PATH_MAX];
static char s_screenshots[PATH_MAX];
static char s_scores[PATH_MAX];
static bool s_roots_resolved = false;

static bool path_join(char *out, size_t capacity, const char *base, const char *relative) {
	int n = snprintf(out, capacity, "%s/%s", base, relative);
	if (n < 0 || (size_t)n >= capacity) {
		if (capacity > 0) out[0] = '\0';
		return false;
	}
	return true;
}

static bool make_directory(const char *path) {
	if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
	return true;
}

static void resolve_roots(void) {
	if (s_roots_resolved) return;

	@autoreleasepool {
		NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(
			NSApplicationSupportDirectory, NSUserDomainMask, YES);
		NSString *base = paths.count > 0 ? paths.firstObject
			: NSHomeDirectory();
		NSString *support = [base stringByAppendingPathComponent:@POP_SUPPORT_DIR_NAME];

		[NSFileManager.defaultManager createDirectoryAtPath:support
			withIntermediateDirectories:YES attributes:nil error:NULL];

		strlcpy(s_support_root, support.fileSystemRepresentation, sizeof(s_support_root));
		path_join(s_game_data, sizeof(s_game_data), s_support_root, "GameData");
		path_join(s_settings, sizeof(s_settings), s_support_root, "Settings");
		path_join(s_saves, sizeof(s_saves), s_support_root, "Saves");
		path_join(s_replays, sizeof(s_replays), s_support_root, "Replays");
		path_join(s_screenshots, sizeof(s_screenshots), s_support_root, "Screenshots");
		path_join(s_scores, sizeof(s_scores), s_support_root, "Scores");

		NSString *res = NSBundle.mainBundle.resourcePath;
		strlcpy(s_resources, res.fileSystemRepresentation, sizeof(s_resources));

		make_directory(s_settings);
		make_directory(s_saves);
		make_directory(s_replays);
		make_directory(s_screenshots);
		make_directory(s_scores);
	}
	s_roots_resolved = true;
}

bool macos_copy_path(MacosPathRoot root, const char *relative, char *out, size_t capacity) {
	if (!s_roots_resolved) resolve_roots();
	const char *base = NULL;
	switch (root) {
		case MACOS_GAME_DATA: base = s_game_data; break;
		case MACOS_RESOURCES: base = s_resources; break;
		case MACOS_SETTINGS: base = s_settings; break;
		case MACOS_SAVES: base = s_saves; break;
		case MACOS_REPLAYS: base = s_replays; break;
		case MACOS_SCREENSHOTS: base = s_screenshots; break;
		case MACOS_SCORES: base = s_scores; break;
		default: return false;
	}
	if (relative == NULL || relative[0] == '\0') {
		if (strlcpy(out, base, capacity) >= capacity) { out[0] = '\0'; return false; }
		return true;
	}
	return path_join(out, capacity, base, relative);
}

// ---------------------------------------------------------------------------
// Native alerts
// ---------------------------------------------------------------------------

void macos_present_error(const char *title, const char *message) {
	NSAlert *alert = [[NSAlert alloc] init];
	alert.alertStyle = NSAlertStyleWarning;
	alert.messageText = title ? @(title) : @"Prince of Persia";
	alert.informativeText = message ? @(message) : @"An error occurred.";
	[alert addButtonWithTitle:NSLocalizedString(@"OK", nil)];
	[alert runModal];
}

// ---------------------------------------------------------------------------
// SHA-256 helpers
// ---------------------------------------------------------------------------

static bool file_sha256(const char *path, unsigned char out[CC_SHA256_DIGEST_LENGTH]) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) return false;
	CC_SHA256_CTX ctx;
	CC_SHA256_Init(&ctx);
	unsigned char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
	}
	int err = ferror(f);
	fclose(f);
	if (err != 0) return false;
	CC_SHA256_Final(out, &ctx);
	return true;
}

static bool sha256_matches(const unsigned char digest[CC_SHA256_DIGEST_LENGTH],
                           const char *hex) {
	char actual[2 * CC_SHA256_DIGEST_LENGTH + 1];
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
		snprintf(&actual[2 * i], 3, "%02x", digest[i]);
	}
	actual[2 * CC_SHA256_DIGEST_LENGTH] = '\0';
	return strcasecmp(actual, hex) == 0;
}

// ---------------------------------------------------------------------------
// DAT table structure validation
// ---------------------------------------------------------------------------

static uint32_t read_le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t read_le16(const uint8_t *p) {
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Validates the DAT table layout: six-byte little-endian header, table bounds,
// entry count/entry bounds and each resource's checksum-byte-plus-payload
// bounds. Uses the actual dat_header_type / dat_table_type / dat_res_type
// layouts, not arbitrary minimum file sizes (GUARD1/2 are only 117 bytes).
static bool validate_dat_structure(const char *path, char *error, size_t error_cap) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		snprintf(error, error_cap, "cannot open %s", path);
		return false;
	}
	struct stat st;
	if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
		fclose(f);
		snprintf(error, error_cap, "cannot stat %s", path);
		return false;
	}
	off_t file_size = st.st_size;

	uint8_t header[6];
	if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
		fclose(f);
		snprintf(error, error_cap, "%s: truncated header", path);
		return false;
	}
	uint32_t table_offset = read_le32(&header[0]);
	uint16_t table_size = read_le16(&header[4]);

	// The table is res_count (u16) followed by res_count * 8-byte entries.
	if (table_size < 2 || ((table_size - 2) % 8) != 0) {
		fclose(f);
		snprintf(error, error_cap, "%s: invalid table size", path);
		return false;
	}
	uint64_t table_start = table_offset;
	uint64_t table_end = table_start + table_size;
	if (table_start < sizeof(header) || table_end > (uint64_t)file_size) {
		fclose(f);
		snprintf(error, error_cap, "%s: table out of bounds", path);
		return false;
	}

	int res_count = (table_size - 2) / 8;
	uint8_t *table = malloc(table_size);
	if (table == NULL) {
		fclose(f);
		snprintf(error, error_cap, "%s: out of memory", path);
		return false;
	}
	if (fseek(f, (long)table_start, SEEK_SET) != 0 ||
	    fread(table, table_size, 1, f) != 1) {
		free(table);
		fclose(f);
		snprintf(error, error_cap, "%s: cannot read table", path);
		return false;
	}
	fclose(f);

	for (int i = 0; i < res_count; ++i) {
		const uint8_t *e = &table[2 + i * 8];
		uint32_t offset = read_le32(&e[2]);
		uint16_t size = read_le16(&e[6]);
		// checksum byte + payload must fit within the file.
		if ((uint64_t)offset + 1 + size > (uint64_t)file_size) {
			free(table);
			snprintf(error, error_cap, "%s: resource %u bounds overflow", path, (unsigned)i);
			return false;
		}
	}
	free(table);
	return true;
}

// ---------------------------------------------------------------------------
// Install manifest
// ---------------------------------------------------------------------------

static void manifest_path(char *out, size_t capacity) {
	path_join(out, capacity, s_game_data, "install-manifest.json");
}

static bool install_is_valid(void) {
	char mpath[PATH_MAX];
	manifest_path(mpath, sizeof(mpath));
	FILE *f = fopen(mpath, "rb");
	if (f == NULL) return false;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	rewind(f);
	if (len <= 0 || len > (1 << 20)) { fclose(f); return false; }
	char *buf = malloc((size_t)len + 1);
	if (buf == NULL) { fclose(f); return false; }
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return false; }
	buf[len] = '\0';
	fclose(f);

	bool valid = false;
	@autoreleasepool {
		NSData *data = [NSData dataWithBytesNoCopy:buf length:(NSUInteger)len freeWhenDone:NO];
		NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
		if ([dict isKindOfClass:NSDictionary.class]) {
			NSString *archiveHash = dict[@"archive_sha256"];
			NSDictionary *files = dict[@"files"];
			if ([archiveHash isKindOfClass:NSString.class] &&
			    [files isKindOfClass:NSDictionary.class] &&
			    [archiveHash caseInsensitiveCompare:@(pop_archive_sha256_hex)] == NSOrderedSame) {
				valid = true;
				for (int i = 0; i < POP_DAT_COUNT && valid; ++i) {
					NSDictionary *entry = files[@(pop_dat_names[i])];
					if (![entry isKindOfClass:NSDictionary.class]) { valid = false; break; }
					NSNumber *size = entry[@"size"];
					NSString *hash = entry[@"sha256"];
					if (![size isKindOfClass:NSNumber.class] || ![hash isKindOfClass:NSString.class]) {
						valid = false; break;
					}
					char fpath[PATH_MAX];
					path_join(fpath, sizeof(fpath), s_game_data, pop_dat_names[i]);
					struct stat st;
					if (stat(fpath, &st) != 0 || st.st_size != (off_t)size.longLongValue) {
						valid = false; break;
					}
					unsigned char digest[CC_SHA256_DIGEST_LENGTH];
					if (!file_sha256(fpath, digest) ||
					    !sha256_matches(digest, hash.UTF8String)) {
						valid = false; break;
					}
				}
			}
		}
	}
	free(buf);
	return valid;
}

// ---------------------------------------------------------------------------
// Downloader (NSURLSession with progress + cancel)
// ---------------------------------------------------------------------------

typedef NS_ENUM(NSInteger, DownloadResult) {
	DownloadSucceeded,
	DownloadCancelled,
	DownloadFailed,
};

@interface MacosDownloader : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic) DownloadResult result;
@property(nonatomic, copy) NSString *errorMessage;
@property(nonatomic, copy, readonly) NSString *destinationPath;
@property(nonatomic, strong) NSURLSession *session;
@property(nonatomic, strong) NSURLSessionDownloadTask *task;
@property(nonatomic, weak) NSProgressIndicator *bar;
@property(nonatomic, copy) NSString *temporaryFile;
@property(nonatomic) long long expectedBytes;
@property(nonatomic) long long receivedBytes;
@property(nonatomic) BOOL cancelledByUser;
@end

@implementation MacosDownloader

- (instancetype)initWithDestination:(NSString *)destination {
	self = [super init];
	if (self) {
		_destinationPath = [destination copy];
		self.result = DownloadFailed;
	}
	return self;
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
willPerformHTTPRedirection:(NSHTTPURLResponse *)response
        newRequest:(NSURLRequest *)request
 completionHandler:(void (^)(NSURLRequest *))completionHandler {
	// Allow ordinary HTTPS redirects (as used by Archive.org); reject any
	// downgrade to plain HTTP.
	if ([request.URL.scheme caseInsensitiveCompare:@"https"] == NSOrderedSame) {
		completionHandler(request);
	} else {
		self.result = DownloadFailed;
		self.errorMessage = @"The download was redirected to an insecure (HTTP) URL; refusing.";
		completionHandler(nil);
	}
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
	self.receivedBytes = totalBytesWritten;
	self.expectedBytes = totalBytesExpectedToWrite;
	if (totalBytesExpectedToWrite > 0) {
		dispatch_async(dispatch_get_main_queue(), ^{
			self.bar.doubleValue = (double)totalBytesWritten / (double)totalBytesExpectedToWrite;
		});
	}
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location {
	NSError *moveError = nil;
	NSFileManager *fm = NSFileManager.defaultManager;
	NSString *dest = self.destinationPath;
	[fm removeItemAtPath:dest error:NULL]; // destination is unique to this attempt
	if ([fm moveItemAtURL:location toURL:[NSURL fileURLWithPath:dest] error:&moveError]) {
		self.temporaryFile = dest;
		self.result = DownloadSucceeded;
	} else {
		self.result = DownloadFailed;
		self.errorMessage = moveError.localizedDescription;
	}
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
	if (error != nil && self.result == DownloadFailed) {
		self.errorMessage = error.localizedDescription;
	} else if (error != nil && self.result != DownloadSucceeded) {
		if (error.code == NSURLErrorCancelled) {
			self.result = DownloadCancelled;
		} else {
			self.result = DownloadFailed;
			self.errorMessage = error.localizedDescription;
		}
	}
}

@end

// Presents the consent disclosure. Returns true only when the user has
// explicitly acknowledged the copyright terms and chosen Download.
static bool present_consent(void) {
	for (;;) {
		NSAlert *alert = [[NSAlert alloc] init];
		alert.alertStyle = NSAlertStyleInformational;
		alert.messageText = NSLocalizedString(@"Download original game data?", nil);
		alert.informativeText = [NSString stringWithFormat:
			@"Prince of Persia needs the original game data files to run.\n\n"
			"Source: %@\n"
			"Approximate download size: 345 KiB.\n\n"
			"The original game assets remain copyrighted. This project does not "
			"grant you a license to the game content; you must have permission to "
			"download and use these files.", POP_ARCHIVE_URL];

		[alert addButtonWithTitle:NSLocalizedString(@"Download Game Data", nil)];
		[alert addButtonWithTitle:NSLocalizedString(@"Quit", nil)];

		NSButton *ack = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 300, 18)];
		ack.buttonType = NSButtonTypeSwitch;
		ack.title = @"I have permission to download and use these files.";
		alert.accessoryView = ack;

		NSModalResponse response = [alert runModal];
		BOOL downloadChosen = (response == NSAlertFirstButtonReturn);
		if (!downloadChosen) return false; // Quit
		if (ack.state == NSControlStateValueOn) return true;

		NSAlert *must = [[NSAlert alloc] init];
		must.alertStyle = NSAlertStyleWarning;
		must.messageText = @"Acknowledgement required";
		must.informativeText = @"Please confirm that you have permission to download and use the original files.";
		[must addButtonWithTitle:@"OK"];
		[must runModal];
	}
}

// Runs the download with a responsive progress/cancel sheet. Returns the
// result and stores the downloaded (but unverified) file at the given path.
static DownloadResult run_download_to(const char *destination_path) {
	@autoreleasepool {
		NSString *dest = @(destination_path);
		MacosDownloader *downloader = [[MacosDownloader alloc] initWithDestination:dest];

		NSURLSessionConfiguration *config = NSURLSessionConfiguration.defaultSessionConfiguration;
		config.timeoutIntervalForRequest = 60.0;
		NSURLSession *session = [NSURLSession sessionWithConfiguration:config
		                                                      delegate:downloader
		                                                 delegateQueue:NSOperationQueue.mainQueue];
		downloader.session = session;

		NSURLRequest *request = [NSURLRequest requestWithURL:[NSURL URLWithString:POP_ARCHIVE_URL]];
		downloader.task = [session downloadTaskWithRequest:request];
		[downloader.task resume];

		NSAlert *alert = [[NSAlert alloc] init];
		alert.messageText = @"Downloading game data…";
		alert.informativeText = [NSString stringWithFormat:@"Downloading from Archive.org."];
		[alert addButtonWithTitle:NSLocalizedString(@"Cancel", nil)];

		NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 300, 20)];
		bar.style = NSProgressIndicatorStyleBar;
		bar.indeterminate = NO;
		bar.minValue = 0.0;
		bar.maxValue = 1.0;
		alert.accessoryView = bar;
		downloader.bar = bar;

		[bar startAnimation:nil];

		NSModalSession modalSession = [NSApp beginModalSessionForWindow:alert.window];
		BOOL userCancelled = NO;
		for (;;) {
			NSModalResponse response = [NSApp runModalSession:modalSession];
			if (response != NSModalResponseContinue) {
				userCancelled = YES;
				break;
			}
			// Pump the main run loop in default mode so main-queue delegate
			// callbacks are serviced while the alert is open.
			[NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
			                      beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
			if (downloader.result == DownloadSucceeded ||
			    downloader.result == DownloadCancelled) {
				break;
			}
			if (downloader.receivedBytes > 0 && downloader.expectedBytes <= 0) {
				bar.indeterminate = YES;
			}
		}
		[NSApp endModalSession:modalSession];
		[bar stopAnimation:nil];

		if (userCancelled) {
			[downloader.task cancel];
		}
		[session finishTasksAndInvalidate];
		downloader.session = nil;

		if (userCancelled) return DownloadCancelled;
		return downloader.result;
	}
}

// ---------------------------------------------------------------------------
// Extraction + install
// ---------------------------------------------------------------------------

static bool remove_tree(const char *path) {
	NSString *p = @(path);
	if (![NSFileManager.defaultManager fileExistsAtPath:p]) return true;
	return [NSFileManager.defaultManager removeItemAtPath:p error:NULL];
}

static bool extract_zip_with_ditto(const char *zip_path, const char *staging_path) {
	NSTask *task = [[NSTask alloc] init];
	task.launchPath = @"/usr/bin/ditto";
	task.arguments = @[@"-x", @"-k", @(zip_path), @(staging_path)];
	task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
	task.standardError = NSFileHandle.fileHandleWithNullDevice;
	@try {
		[task launch];
		[task waitUntilExit];
	}
	@catch (NSException *exception) {
		return false;
	}
	return task.terminationStatus == 0;
}

static bool verify_downloaded_payload(const char *path, char *error, size_t error_cap) {
	struct stat st;
	if (stat(path, &st) != 0 || st.st_size != (off_t)POP_ARCHIVE_BYTES) {
		snprintf(error, error_cap,
		         "Downloaded file has an unexpected size (got %lld bytes, expected %zu).",
		         (long long)st.st_size, POP_ARCHIVE_BYTES);
		return false;
	}
	unsigned char digest[CC_SHA256_DIGEST_LENGTH];
	if (!file_sha256(path, digest)) {
		snprintf(error, error_cap, "Could not read the downloaded file.");
		return false;
	}
	if (!sha256_matches(digest, pop_archive_sha256_hex)) {
		snprintf(error, error_cap, "Downloaded file failed the SHA-256 integrity check.");
		return false;
	}
	return true;
}

// Copies exactly the 21 required DAT files from the staging directory into
// the candidate directory, computes the manifest, and validates each DAT's
// structure. Leaves the candidate untouched on failure.
static bool publish_candidate_from_staging(const char *staging_path,
                                           const char *candidate_path,
                                           char *error, size_t error_cap) {
	if (mkdir(candidate_path, 0755) != 0) {
		snprintf(error, error_cap, "Could not create the install directory.");
		return false;
	}

	NSMutableDictionary *filesDict = [NSMutableDictionary dictionary];
	for (int i = 0; i < POP_DAT_COUNT; ++i) {
		const char *name = pop_dat_names[i];
		char src[PATH_MAX], dst[PATH_MAX];
		char validate_error[256] = "";
		snprintf(src, sizeof(src), "%s/%s", staging_path, name);
		snprintf(dst, sizeof(dst), "%s/%s", candidate_path, name);

		if (!validate_dat_structure(src, validate_error, sizeof(validate_error))) {
			snprintf(error, error_cap, "%s is invalid: %s", name, validate_error);
			return false;
		}
		if (![NSFileManager.defaultManager copyItemAtPath:@(src) toPath:@(dst) error:NULL]) {
			snprintf(error, error_cap, "Could not copy %s.", name);
			return false;
		}
		struct stat st;
		if (stat(dst, &st) != 0) {
			snprintf(error, error_cap, "Could not stat %s after copying.", name);
			return false;
		}
		unsigned char digest[CC_SHA256_DIGEST_LENGTH];
		if (!file_sha256(dst, digest)) {
			snprintf(error, error_cap, "Could not hash %s.", name);
			return false;
		}
		char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
		for (int j = 0; j < CC_SHA256_DIGEST_LENGTH; ++j) {
			snprintf(&hex[2 * j], 3, "%02x", digest[j]);
		}
		hex[2 * CC_SHA256_DIGEST_LENGTH] = '\0';
		filesDict[@(name)] = @{ @"size": @(st.st_size), @"sha256": @(hex) };
	}

	NSDictionary *manifest = @{
		@"format": @1,
		@"archive_url": POP_ARCHIVE_URL,
		@"archive_sha256": @(pop_archive_sha256_hex),
		@"files": filesDict,
	};
	NSError *jsonError = nil;
	NSData *json = [NSJSONSerialization dataWithJSONObject:manifest
	                                              options:NSJSONWritingPrettyPrinted
	                                                error:&jsonError];
	if (json == nil) {
		snprintf(error, error_cap, "Could not encode the install manifest: %s",
		         jsonError.localizedDescription.UTF8String ?: "unknown error");
		return false;
	}
	char mpath[PATH_MAX];
	snprintf(mpath, sizeof(mpath), "%s/install-manifest.json", candidate_path);
	if (![json writeToFile:@(mpath) atomically:YES]) {
		snprintf(error, error_cap, "Could not write the install manifest.");
		return false;
	}
	return true;
}

// Publishes the fully-validated candidate directory to GameData via a
// same-volume rename. A prior valid install is never lost: when replacing an
// existing (invalid) GameData, we move the old tree to a backup first and
// restore it if the new rename fails.
static bool publish_candidate(const char *candidate_path, char *error, size_t error_cap) {
	char backup[PATH_MAX];
	snprintf(backup, sizeof(backup), "%s/.deleted-%ld", s_support_root, (long)getpid());

	bool had_existing = false;
	struct stat st;
	if (stat(s_game_data, &st) == 0) {
		had_existing = true;
		if (rename(s_game_data, backup) != 0) {
			snprintf(error, error_cap, "Could not move the existing game data aside.");
			return false;
		}
	}
	if (rename(candidate_path, s_game_data) != 0) {
		if (had_existing) rename(backup, s_game_data); // restore
		snprintf(error, error_cap, "Could not publish the downloaded game data.");
		return false;
	}
	remove_tree(backup);
	return true;
}

// ---------------------------------------------------------------------------
// Top-level acquisition gate
// ---------------------------------------------------------------------------

static void install_game_menu(void);

bool macos_prepare_application(void) {
	@autoreleasepool {
		// Canonicalize argv[0] to the bundle executable's absolute path; the
		// settings layer hashes it to invalidate SDLPoP.cfg when the app changes.
		if (g_argc > 0 && g_argv != NULL && g_argv[0] != NULL) {
			char *resolved = realpath(g_argv[0], NULL);
			if (resolved != NULL) {
				g_argv[0] = strdup(resolved);
				free(resolved);
			}
		}
		// 1. Initialize SDL video first: this creates SDLApplication, its
		//    delegate and the Cocoa event pumping used by set_gr_mode later.
		if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
			if (SDL_Init(SDL_INIT_VIDEO) != 0) {
				macos_present_error("Could not initialize video", SDL_GetError());
				return false;
			}
		}
		// Install the native Game menu (Save/Load/Quicksave/Quickload).
		install_game_menu();
		// 2. Resolve the storage roots beneath Application Support.
		resolve_roots();

		// 3. Completed install (verified manifest + DATs)? Launch offline.
		if (install_is_valid()) {
			return true;
		}

		// 4. Requires consent before any network activity.
		if (!present_consent()) {
			return false; // declined; no network request was made
		}

		// 5. Download + install, with an explicit Retry/Quit loop on failure.
		for (;;) {
			char *staging = NULL, *candidate = NULL, *download_file = NULL;
			NSString *unique = [NSUUID.UUID UUIDString];
			NSString *tmp = NSTemporaryDirectory();
			NSString *stagingPath = [tmp stringByAppendingPathComponent:
				[NSString stringWithFormat:@"pop-staging-%@", unique]];
			NSString *candidatePath = [@(s_support_root) stringByAppendingPathComponent:
				[NSString stringWithFormat:@".gamedata-candidate-%@", unique]];
			NSString *downloadPath = [tmp stringByAppendingPathComponent:
				[NSString stringWithFormat:@"pop-download-%@.zip", unique]];
			NSString *downloadParent = [downloadPath stringByDeletingLastPathComponent];

			[NSFileManager.defaultManager createDirectoryAtPath:downloadParent
				withIntermediateDirectories:YES attributes:nil error:NULL];
			[NSFileManager.defaultManager createDirectoryAtPath:stagingPath
				withIntermediateDirectories:NO attributes:nil error:NULL];

			staging = strdup(stagingPath.fileSystemRepresentation);
			candidate = strdup(candidatePath.fileSystemRepresentation);
			download_file = strdup(downloadPath.fileSystemRepresentation);

			char error[512] = "";
			DownloadResult result = run_download_to(download_file);

			if (result == DownloadSucceeded) {
				if (verify_downloaded_payload(download_file, error, sizeof(error))) {
					if (extract_zip_with_ditto(download_file, staging)) {
						if (publish_candidate_from_staging(staging, candidate, error, sizeof(error)) &&
						    publish_candidate(candidate, error, sizeof(error))) {
							// Success.
							free(staging); free(candidate); free(download_file);
							return install_is_valid();
						}
					} else {
						snprintf(error, sizeof(error), "Could not extract the downloaded archive.");
					}
				}
			} else if (result == DownloadCancelled) {
				snprintf(error, sizeof(error), "Download cancelled.");
			} else {
				snprintf(error, sizeof(error), "Download failed.");
			}

			// Failure: clean up this attempt's staging/candidate/temp files.
			// A prior valid install is never touched during a failed attempt.
			if (staging) remove_tree(staging);
			if (candidate) remove_tree(candidate);
			if (download_file) {
				NSString *d = @(download_file);
				if ([NSFileManager.defaultManager fileExistsAtPath:d]) {
					[NSFileManager.defaultManager removeItemAtPath:d error:NULL];
				}
			}
			free(staging); free(candidate); free(download_file);

			NSAlert *failed = [[NSAlert alloc] init];
			failed.alertStyle = NSAlertStyleWarning;
			failed.messageText = @"Could not install game data";
			failed.informativeText = [NSString stringWithFormat:@"%s", error];
			[failed addButtonWithTitle:NSLocalizedString(@"Retry", nil)];
			[failed addButtonWithTitle:NSLocalizedString(@"Quit", nil)];
			if ([failed runModal] != NSAlertFirstButtonReturn) {
				return false;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Native Game menu + save/load panels
// ---------------------------------------------------------------------------

@interface MacosMenuTarget : NSObject
@end

static MacosMenuTarget *g_menu_target;

@implementation MacosMenuTarget
- (void)saveGame:(id)sender { game_request_command(GAME_SAVE_NAMED); }
- (void)loadGame:(id)sender { game_request_command(GAME_LOAD_NAMED); }
- (void)quicksave:(id)sender { game_request_command(GAME_QUICKSAVE); }
- (void)quickload:(id)sender { game_request_command(GAME_QUICKLOAD); }
@end

static NSString *function_key(unichar code) {
	return [NSString stringWithFormat:@"%C", code];
}

static void install_game_menu(void) {
	if (NSApp == nil) return;
	NSMenu *mainMenu = NSApp.mainMenu;
	if (mainMenu == nil) {
		mainMenu = [[NSMenu alloc] initWithTitle:@""];
		NSApp.mainMenu = mainMenu;
	}

	// App menu (Quit keeps SDL's quit path via terminate:).
	NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"Prince of Persia" action:nil keyEquivalent:@""];
	NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Prince of Persia"];
	[appMenu addItemWithTitle:NSLocalizedString(@"Quit Prince of Persia", nil)
	                   action:@selector(terminate:) keyEquivalent:@"q"];
	appItem.submenu = appMenu;
	[mainMenu addItem:appItem];

	// Game menu.
	g_menu_target = [[MacosMenuTarget alloc] init];
	NSMenuItem *gameItem = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Game", nil) action:nil keyEquivalent:@""];
	NSMenu *gameMenu = [[NSMenu alloc] initWithTitle:@"Game"];

	NSMenuItem *save = [gameMenu addItemWithTitle:NSLocalizedString(@"Save Game…", nil)
	                                       action:@selector(saveGame:) keyEquivalent:@"s"];
	[save setTarget:g_menu_target];
	NSMenuItem *load = [gameMenu addItemWithTitle:NSLocalizedString(@"Load Game…", nil)
	                                       action:@selector(loadGame:) keyEquivalent:@"o"];
	[load setTarget:g_menu_target];
	NSMenuItem *qsave = [gameMenu addItemWithTitle:NSLocalizedString(@"Quicksave", nil)
	                                        action:@selector(quicksave:)
	                                 keyEquivalent:function_key(NSF6FunctionKey)];
	[qsave setTarget:g_menu_target];
	NSMenuItem *qload = [gameMenu addItemWithTitle:NSLocalizedString(@"Quickload", nil)
	                                        action:@selector(quickload:)
	                                 keyEquivalent:function_key(NSF9FunctionKey)];
	[qload setTarget:g_menu_target];

	gameItem.submenu = gameMenu;
	[mainMenu addItem:gameItem];
}

void macos_run_game_panel(uint64_t generation, bool is_save, void (*pump)(void)) {
	NSSavePanel *panel = is_save ? [NSSavePanel savePanel] : (NSSavePanel *)[NSOpenPanel openPanel];
	UTType *popsave = [UTType typeWithFilenameExtension:@"popsave"];
	panel.allowedContentTypes = @[popsave];
	panel.allowsOtherFileTypes = NO;
	panel.canCreateDirectories = YES;
	if (is_save) {
		panel.nameFieldStringValue = @"game.popsave";
	}

	char saves[PATH_MAX];
	if (macos_copy_path(MACOS_SAVES, "", saves, sizeof(saves))) {
		panel.directoryURL = [NSURL fileURLWithPath:@(saves)];
	}

	// Make the cursor visible and clickable while the panel is up, then run the
	// panel as an app-modal dialog (Cocoa's own run loop keeps it responsive;
	// the game frame stays frozen behind it).
	SDL_ShowCursor(SDL_ENABLE);
	NSModalResponse response = [panel runModal];

	bool cancelled = (response != NSModalResponseOK);
	NSString *chosen = nil;
	if (!cancelled) {
		chosen = panel.URL.path;
	}
	game_complete_panel(generation, cancelled ? NULL : chosen.UTF8String, cancelled);
}
