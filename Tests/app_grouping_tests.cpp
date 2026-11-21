#include <cstdio>
#include "Capture/AppGrouping.h"
using namespace pb;
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main()
{
    CHECK (groupIdForBundleId ("com.google.Chrome") == "com.google.Chrome");
    CHECK (groupIdForBundleId ("com.google.Chrome.helper") == "com.google.Chrome");
    CHECK (groupIdForBundleId ("com.google.Chrome.helper.Renderer") == "com.google.Chrome");
    CHECK (groupIdForBundleId ("com.google.Chrome.helper.GPU") == "com.google.Chrome");
    CHECK (groupIdForBundleId ("com.microsoft.edgemac.helper") == "com.microsoft.edgemac");
    CHECK (groupIdForBundleId ("com.tinyspeck.slackmacgap.helper.Renderer") == "com.tinyspeck.slackmacgap");
    CHECK (groupIdForBundleId ("com.apple.WebKit.GPU") == "com.apple.Safari");
    CHECK (groupIdForBundleId ("com.apple.WebKit.WebContent") == "com.apple.Safari");
    CHECK (groupIdForBundleId ("com.apple.Safari") == "com.apple.Safari");
    CHECK (groupIdForBundleId ("com.apple.Music") == "com.apple.Music");
    CHECK (groupIdForBundleId ("com.spotify.client") == "com.spotify.client");
    CHECK (groupIdForBundleId ("com.spotify.client.helper") == "com.spotify.client");
    CHECK (groupIdForBundleId ("com.example.helpertool") == "com.example.helpertool");   // ".helper" must be a whole component
    CHECK (groupIdForBundleId ("com.example.helperapp.main") == "com.example.helperapp.main");
    CHECK (groupIdForBundleId (".helper") == ".helper");                                   // degenerate: unchanged
    CHECK (groupIdForBundleId ("").empty());
    CHECK (processBelongsToGroup ("com.google.Chrome.helper.Renderer", "com.google.Chrome"));
    CHECK (processBelongsToGroup ("com.google.Chrome", "com.google.Chrome"));
    CHECK (! processBelongsToGroup ("com.google.Chrome.canary", "com.google.Chrome"));     // different app: no prefix matching
    CHECK (! processBelongsToGroup ("com.google.Chrome", ""));
    CHECK (fallbackAppName ("com.spotify.client") == "client" && fallbackAppName ("plain") == "plain" && fallbackAppName ("a.") == "a.");
    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
