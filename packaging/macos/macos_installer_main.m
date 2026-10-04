#import <Cocoa/Cocoa.h>

// Launch the bundled installer directly. Opening a .command file asks Terminal
// to type a command into the user's interactive shell, whose startup hooks can
// interfere with that command. Keep all payloads inside the app so Gatekeeper
// app translocation does not break paths to files beside the download.
int main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSURL *script = [[NSBundle mainBundle] URLForResource:@"install-macos"
                                              withExtension:@"command"];
        NSMutableArray<NSString *> *arguments = [NSMutableArray array];
        if (script) [arguments addObject:script.path];
        for (int i = 1; i < argc; ++i)
            [arguments addObject:[NSString stringWithUTF8String:argv[i]]];

        NSString *failure = nil;
        int status = 1;
        if (!script) {
            failure = @"The installer is incomplete. Download and unzip it again.";
        } else {
            NSTask *task = [[NSTask alloc] init];
            task.executableURL = [NSURL fileURLWithPath:@"/bin/bash"];
            task.arguments = arguments;
            NSMutableDictionary *environment = [[[NSProcessInfo processInfo] environment] mutableCopy];
            [environment removeObjectForKey:@"BASH_ENV"];
            [environment removeObjectForKey:@"ENV"];
            task.environment = environment;
            task.standardInput = [NSFileHandle fileHandleWithNullDevice];
            NSPipe *output = [NSPipe pipe];
            task.standardOutput = output;
            task.standardError = output;
            NSError *error = nil;
            if ([task launchAndReturnError:&error]) {
                // Drain before waiting so lengthy diagnostics cannot fill the pipe.
                NSData *data = [output.fileHandleForReading readDataToEndOfFile];
                [task waitUntilExit];
                status = task.terminationStatus;
                if (data.length) fwrite(data.bytes, 1, data.length, stdout);
                if (status != 0)
                    failure = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            } else {
                failure = error.localizedDescription;
            }
        }
        if (status != 0) {
            if (failure.length == 0) failure = @"Installation could not be completed.";
            fprintf(stderr, "%s\n", failure.UTF8String);
            // Command-line checks return diagnostics without opening a dialog.
            if (argc == 1) {
                [NSApplication sharedApplication];
                [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
                [NSApp activateIgnoringOtherApps:YES];
                NSAlert *alert = [[NSAlert alloc] init];
                alert.messageText = @"LumiPaint setup could not finish";
                alert.informativeText = failure;
                [alert runModal];
            }
        }
        return status;
    }
}
