// macOS implementation of the small OS services (file dialogs, Finder).
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach-o/dyld.h>

#include <string>
#include <vector>

#include "platform.h"

namespace {
// "*.wav;*.sfz" -> {"wav", "sfz"}; "*.*" or "*" -> {} (any file)
NSArray<UTType*>* typesFor(const std::vector<std::pair<std::string, std::string>>& filter) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (const auto& f : filter) {
        NSString* pats = [NSString stringWithUTF8String:f.second.c_str()];
        for (NSString* p in [pats componentsSeparatedByString:@";"]) {
            NSString* ext = [[p stringByReplacingOccurrencesOfString:@"*." withString:@""]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if (ext.length == 0 || [ext isEqualToString:@"*"]) return @[];
            if (UTType* t = [UTType typeWithFilenameExtension:ext]) [types addObject:t];
        }
    }
    return types;
}
std::string utf8(NSURL* url) { return url.path ? std::string(url.path.UTF8String) : std::string(); }
}  // namespace

namespace platform {

std::vector<std::string> openFiles(const std::vector<std::pair<std::string, std::string>>& filter, bool multi) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = multi;
        NSArray<UTType*>* types = typesFor(filter);
        if (types.count > 0) panel.allowedContentTypes = types;
        std::vector<std::string> out;
        if ([panel runModal] == NSModalResponseOK)
            for (NSURL* url in panel.URLs) out.push_back(utf8(url));
        return out;
    }
}

std::string pickFolder(const std::string& title) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.message = [NSString stringWithUTF8String:title.c_str()];
        if ([panel runModal] == NSModalResponseOK) return utf8(panel.URL);
        return {};
    }
}

std::string saveFile(const std::vector<std::pair<std::string, std::string>>& filter, const std::string& defaultName,
                     const std::string& defaultExt) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        NSArray<UTType*>* types = typesFor(filter);
        if (types.count > 0) panel.allowedContentTypes = types;
        std::string name = defaultName;
        if (!defaultExt.empty() && name.find('.') == std::string::npos) name += "." + defaultExt;
        panel.nameFieldStringValue = [NSString stringWithUTF8String:name.c_str()];
        if ([panel runModal] == NSModalResponseOK) return utf8(panel.URL);
        return {};
    }
}

void openInExplorer(const std::string& path) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        BOOL dir = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:url.path isDirectory:&dir] && dir)
            [[NSWorkspace sharedWorkspace] openURL:url];
        else
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
    }
}

std::string executableDir() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string p(size, '\0');
    if (_NSGetExecutablePath(p.data(), &size) != 0) return ".";
    p.resize(strlen(p.c_str()));
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

}  // namespace platform
