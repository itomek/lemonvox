"""Isolated path test using the pinned upstream selector; no app launch or TCC access.
Usage: python3 tests/app_name_migration_test.py SUInstaller.m ConfigCommon.xcconfig NEW_APP
The file-operation phase models upgrade order; it is not a full Sparkle/Homebrew install.
"""
import hashlib, pathlib, plistlib, shutil, subprocess, sys, tempfile
source, config, app = map(pathlib.Path, sys.argv[1:])
assert hashlib.sha256(source.read_bytes()).hexdigest() == '30d38f074235b0bbfc21326c5f5b8710444e2e03f740411426eee29fbf6c3532'
assert hashlib.sha256(config.read_bytes()).hexdigest() == '1b17b8791eb24e90d48ef2dedd3b316d1711a65e0269cba42b19288ea1d7012e'
assert 'SPARKLE_NORMALIZE_INSTALLED_APPLICATION_NAME = 0' in config.read_text()
s = source.read_text()
selector = s[s.index('+ (nullable NSString *)installSourcePathInUpdateFolder:'):s.index('+ (nullable id<SUInstallerProtocol>)installerForHost:')]
start = s.index('            NSString *normalizedInstallationPath = nil;')
end = s.index('            installer = [[SUPlainInstaller alloc]', start)
destination = s[start:end]
header = '''#import <Foundation/Foundation.h>
#define SPARKLE_BUILD_PACKAGE_SUPPORT 0
#define SPARKLE_NORMALIZE_INSTALLED_APPLICATION_NAME 0
#define SULog(...) ((void)0)
@interface SUHost : NSObject
@property NSString *bundlePath;
@property NSString *name;
@property NSBundle *bundle;
@end
@implementation SUHost
@end
static NSString *SUNormalizedInstallationPath(SUHost *host) { abort(); }
@interface Selector : NSObject
@end
@implementation Selector
'''
main = '''
@end
int main(int argc, const char **argv) { @autoreleasepool {
SUHost *host = [SUHost new]; host.bundlePath = @(argv[1]); host.name = @"LemonVox";
host.bundle = [NSBundle bundleWithPath:host.bundlePath];
NSString *selected = [Selector installSourcePathInUpdateFolder:@(argv[2]) forHost:host];
NSCAssert([selected isEqualToString:[@(argv[2]) stringByAppendingPathComponent:@"LemonVox.app"]], @"New bundle must be discoverable");
DESTINATION
NSCAssert([installationPath isEqualToString:host.bundlePath], @"Default installer retains old path");
printf("%s\\n", installationPath.UTF8String);
} return 0; }
'''.replace('DESTINATION', destination)
with tempfile.TemporaryDirectory(prefix='lemonvox-name-migration-') as directory:
    root = pathlib.Path(directory)
    incoming = root/'incoming'; incoming.mkdir()
    subprocess.run(['ditto', str(app), str(incoming/'LemonVox.app')], check=True)
    old = root/'Applications'/'LemonVox Prototype.app'
    old.mkdir(parents=True)
    info = plistlib.loads((app/'Contents/Info.plist').read_bytes())
    old_info = dict(info, CFBundleName='LemonVox Prototype', CFBundleVersion='4')
    (old/'Contents').mkdir()
    (old/'Contents/Info.plist').write_bytes(plistlib.dumps(old_info))
    prefs = root/'Library/Preferences'/f"{info['CFBundleIdentifier']}.plist"
    prefs.parent.mkdir(parents=True)
    sentinel = plistlib.dumps({'trigger': 'sentinel', 'cleanup': 'keep'})
    prefs.write_bytes(sentinel)
    objc = root/'selector.m'; objc.write_text(header+selector+main)
    binary = root/'selector'
    subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Foundation', str(objc), '-o', str(binary)], check=True)
    result = subprocess.check_output([str(binary), str(old), str(incoming)], text=True).strip()
    assert result == str(old)
    # Model Sparkle's selected destination, without invoking its installer or relauncher.
    shutil.rmtree(old)
    subprocess.run(['ditto', str(incoming/'LemonVox.app'), str(old)], check=True)
    assert not (old.parent/'LemonVox.app').exists()
    assert plistlib.loads((old/'Contents/Info.plist').read_bytes())['CFBundleName']=='LemonVox'
    assert prefs.read_bytes()==sentinel
    # Model Homebrew's documented/source-reviewed order: back up/remove old, install new.
    shutil.move(str(old), str(root/'old-backup.app'))
    final = old.parent/'LemonVox.app'
    subprocess.run(['ditto', str(incoming/'LemonVox.app'), str(final)], check=True)
    assert list(final.parent.glob('*.app')) == [final]
    assert prefs.read_bytes()==sentinel
    assert plistlib.loads((final/'Contents/Info.plist').read_bytes())['CFBundleIdentifier']==old_info['CFBundleIdentifier']
print('PASS: pinned Sparkle selector accepts renamed bundle and retains old destination; staged migration model ends at LemonVox.app with one app and unchanged preference-domain fixture')
print('LIMIT: not a real updater/cask transaction, signed identity, TCC continuity, or relaunch test')
