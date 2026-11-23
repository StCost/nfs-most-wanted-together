// Story / boss movies in co-op (src/storyutil.h): the client must play a beaten rival's
// video itself. Shared skip, watch-along and leftover SkipIntro/SKIPMOVIES must not eat it.
#include "mwt_test.h"
#include "../src/storyutil.h"
#include "../src/togetherutil.h"

using namespace StorySync;
using TogetherUtil::WidescreenFixLine;
using TogetherUtil::PatchWidescreenFixIniText;

TEST(story_blacklist_movie_is_played_by_each_career) {
    CHECK(IsBlacklistRivalMovie("blacklist_10"));
    CHECK(IsStoryMovieName("blacklist_10"));
    CHECK(IsStoryMovieName("storyfmv_pin11"));
    CHECK(!IsBlacklistRivalMovie("storyfmv_pin11"));
    // watch-along / world hold never take a blacklist rival movie
    CHECK(!MayWatchAlong(2, true, false, true, false, false));
    CHECK(MayWatchAlong(2, false, false, true, false, false));
    CHECK(!MayWatchAlong(1, false, false, true, false, false));
    CHECK(!MayHoldWorldForPeer(true, false, true, true, false, false));
    CHECK(MayHoldWorldForPeer(true, false, true, false, false, false));
    CHECK(!MayHoldWorldForPeer(true, true, true, false, false, false));
}

TEST(story_skipmovies_and_wsf_skipintro_are_cleared) {
    CHECK(ShouldClearSkipMovies(true, false));
    CHECK(ShouldClearSkipMovies(false, true));
    CHECK(ShouldClearSkipMovies(true, true));
    CHECK(!ShouldClearSkipMovies(false, false));
    std::string ini = "[MISC]\r\nSkipIntro = 1 ; skips all movies\r\nFOV = 1\r\n";
    CHECK(PatchWidescreenFixIniText(ini));
    CHECK(ini.find("SkipIntro = 0") != std::string::npos);
    CHECK(ini.find("FOV = 1") != std::string::npos);
    CHECK(!PatchWidescreenFixIniText(ini));   // already 0
}

TEST(story_career_movie_retries_if_it_ends_immediately) {
    using MC = MovieClose;
    CHECK(ShouldRetryCareerMovie(0, MC::MovieDone, false, false, false));
    CHECK(ShouldRetryCareerMovie(400, MC::MovieDone, false, false, false));
    CHECK(!ShouldRetryCareerMovie(2000, MC::MovieDone, false, false, false));
    CHECK(!ShouldRetryCareerMovie(0, MC::Skipped, false, false, false));
    CHECK(!ShouldRetryCareerMovie(0, MC::MovieDone, true, false, false));    // SKIPMOVIES still on
    CHECK(!ShouldRetryCareerMovie(0, MC::MovieDone, false, true, false));    // already retried
    CHECK(!ShouldRetryCareerMovie(0, MC::MovieDone, false, false, true));    // player voted skip
    CHECK(QuietStreamerForMovie(true));
    CHECK(!QuietStreamerForMovie(false));
}

TEST(story_other_player_finishing_does_not_auto_skip_ours) {
    const uint32_t movie = 0x11223344u;
    // both on the same movie, no votes yet: our skip button is held
    CHECK(!AllowSkipButton(true, true, movie, movie, 0, 0));
    // both voted / force-skip agreed
    CHECK(AllowSkipButton(true, true, movie, movie, movie, 0));
    CHECK(AllowSkipButton(true, true, movie, movie, 0, movie));
    // the other player already left (finished their copy / in SMS): watching alone, do not auto-skip;
    // a local skip button is allowed, but nothing forces one
    CHECK(AllowSkipButton(true, true, movie, 0, 0, 0));
    CHECK(AllowSkipButton(true, true, movie, 0xABCDu, 0, 0));
    CHECK(AllowSkipButton(false, true, movie, movie, 0, 0));
    CHECK(AllowSkipButton(true, false, movie, movie, 0, 0));
}
