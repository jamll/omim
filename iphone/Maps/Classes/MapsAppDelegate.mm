#import "MapsAppDelegate.h"
#import "MapViewController.h"
#import "SettingsManager.h"
#import "Preferences.h"
#import "LocationManager.h"

#include <sys/xattr.h>

#include "../../../platform/settings.hpp"

@implementation MapsAppDelegate

@synthesize m_window;
@synthesize m_mapViewController;
@synthesize m_locationManager;

#define V2_0_TAG "2.0"
#define FIRST_LAUNCH_KEY "FirstLaunchOnVersion"

+ (MapsAppDelegate *) theApp
{
  return [[UIApplication sharedApplication] delegate];
}

- (void) onFirstLaunchCheck
{
  // Called only for V1.0.1 -> V2.0 upgrade
  string v;
  if (!Settings::Get(FIRST_LAUNCH_KEY, v))
  {
    // Scan all files in the Documents directory and do necessary actions with them
    NSArray * paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString * documentsDirectoryPath = [paths objectAtIndex:0];
    NSFileManager * fileManager = [NSFileManager defaultManager];

    // Delete old travel guide temporary files
    [fileManager removeItemAtPath:[documentsDirectoryPath stringByAppendingPathComponent:@"index.stamp"] error:nil];
    [fileManager removeItemAtPath:[documentsDirectoryPath stringByAppendingPathComponent:@"index.idx"] error:nil];

    // Delete ".downloading" and ".resume" files and disable iCloud backup for downloaded ".mwm" maps
    NSArray * files = [fileManager contentsOfDirectoryAtPath:documentsDirectoryPath error:nil];
    for (NSUInteger i = 0; i < [files count]; ++i)
    {
      NSString * fileName = [files objectAtIndex:i];

      if ([fileName rangeOfString:@".downloading"].location != NSNotFound)
        [fileManager removeItemAtPath:[documentsDirectoryPath stringByAppendingPathComponent:fileName] error:nil];
      else if ([fileName rangeOfString:@".resume"].location != NSNotFound)
          [fileManager removeItemAtPath:[documentsDirectoryPath stringByAppendingPathComponent:fileName] error:nil];
      else if ([fileName rangeOfString:@".mwm"].location != NSNotFound)
      { // Disable iCloud backup
        static char const * attrName = "com.apple.MobileBackup";
        u_int8_t attrValue = 1;
        int result = setxattr([[documentsDirectoryPath stringByAppendingPathComponent:fileName] UTF8String], attrName, &attrValue, sizeof(attrValue), 0, 0);
        if (result == 0)
          NSLog(@"Disabled iCloud backup for %@", fileName);
        else
          NSLog(@"Error %d while disabling iCloud backup for %@", errno, fileName);
      }
    }

    Settings::Set(FIRST_LAUNCH_KEY, string(V2_0_TAG));
  }
}

- (void) applicationWillTerminate: (UIApplication *) application
{
	[m_mapViewController OnTerminate];
}

- (void) applicationDidEnterBackground: (UIApplication *) application
{
	[m_mapViewController OnEnterBackground];
}

- (void) applicationWillEnterForeground: (UIApplication *) application
{
  [m_mapViewController OnEnterForeground];
}

- (void) applicationDidFinishLaunching: (UIApplication *) application
{
  [self onFirstLaunchCheck];

  [Preferences setup:m_mapViewController];
  m_locationManager = [[LocationManager alloc] init];

  [m_window addSubview:m_mapViewController.view];
  [m_window makeKeyAndVisible];
}

- (SettingsManager *)settingsManager
{
  if (!m_settingsManager)
    m_settingsManager = [[SettingsManager alloc] init];
  return m_settingsManager;
}

- (void) dealloc
{
  [m_locationManager release];
  [m_settingsManager release];
  m_mapViewController = nil;
  m_window = nil;
  [super dealloc];
}

- (void) disableStandby
{
  ++m_standbyCounter;
  [UIApplication sharedApplication].idleTimerDisabled = YES;
}

- (void) enableStandby
{
  --m_standbyCounter;
  if (m_standbyCounter <= 0)
  {
    [UIApplication sharedApplication].idleTimerDisabled = NO;
    m_standbyCounter = 0;
  }
}

- (void) disableDownloadIndicator
{
  --m_activeDownloadsCounter;
  if (m_activeDownloadsCounter <= 0)
  {
    [UIApplication sharedApplication].networkActivityIndicatorVisible = NO;
    m_activeDownloadsCounter = 0;
  }
}

- (void) enableDownloadIndicator
{
  ++m_activeDownloadsCounter;
  [UIApplication sharedApplication].networkActivityIndicatorVisible = YES;
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application
{
  NSLog(@"applicationDidReceiveMemoryWarning");
}

@end
