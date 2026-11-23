#pragma once
#include <cstdint>
#include <cstring>

// Story movies and shared skip / watch-along (src/story.inl). After a Blacklist boss each career
// must play its own rival movie. The other player's progress must not skip or replace it.
namespace StorySync {
    inline bool IsStoryMovieName(const char* s) {
        return s && (!strncmp(s, "blacklist_", 10) || !strncmp(s, "storyfmv_", 9));
    }
    inline bool IsBlacklistRivalMovie(const char* s) { return s && !strncmp(s, "blacklist_", 10); }

    // Watch-along (SyncStoryScenes=2) is only for non-blacklist story FMVs. Each player plays a
    // beaten rival's movie from their own career sequence.
    inline bool MayWatchAlong(int syncStoryScenes, bool isBlacklistMovie, bool justWatched,
                              bool roaming, bool inPursuit, bool alreadyStarted) {
        return syncStoryScenes >= 2 && !isBlacklistMovie && !justWatched && roaming &&
               !inPursuit && !alreadyStarted;
    }

    // Do not hold the world for a Blacklist rival movie: the other player often finishes it first
    // and would otherwise stand frozen in free roam.
    inline bool MayHoldWorldForPeer(bool peerWatching, bool weWatching, bool roaming,
                                    bool isBlacklistMovie, bool justWatched, bool pendingStart) {
        return peerWatching && !weWatching && roaming && !isBlacklistMovie &&
               !justWatched && !pendingStart;
    }

    // Together skips the boot intro itself. A leftover Widescreen Fix SkipIntro=1, or the game's
    // SKIPMOVIES flag it sets, would end every story movie (including the boss video) at once.
    inline bool ShouldClearSkipMovies(bool wsfSkipIntro, bool skipMoviesMemory) {
        return wsfSkipIntro || skipMoviesMemory;
    }

    // StreamAroundPeer uses the track streamer's pool; in-game movies allocate from the same pool
    // once the streamer is idle. Quiet the peer slot for our own career movies too, not only ones
    // we started to watch along, or the movie can fail to open and the career jumps to SMS.
    inline bool QuietStreamerForMovie(bool storyMovieStartingOrPlaying) {
        return storyMovieStartingOrPlaying;
    }

    enum class MovieClose : int { None = 0, MovieDone = 1, Skipped = 2 };

    // If a career story / boss movie ends immediately (the file could not open because the
    // streamer was still busy, or SKIPMOVIES was just cleared), retry once after a short quiet
    // period. Never retry a skip the player asked for, or a second time.
    inline bool ShouldRetryCareerMovie(unsigned elapsedMs, MovieClose close, bool skipMoviesStillOn,
                                       bool alreadyRetried, bool playerVotedSkip) {
        if (alreadyRetried || skipMoviesStillOn || playerVotedSkip) return false;
        if (close != MovieClose::MovieDone) return false;
        return elapsedMs < 1500u;
    }

    // Shared skip: a skip button is only honoured when both voted (or we already agreed to
    // force-skip). The other player having already finished their own copy is not a reason to
    // swallow or auto-skip ours while we did not vote. Watching alone (peer not on this movie)
    // stays vanilla: the local skip goes through.
    inline bool AllowSkipButton(bool syncOn, bool connected, uint32_t localMovie, uint32_t peerMovie,
                                uint32_t peerVote, uint32_t forceSkip) {
        if (!syncOn || !connected) return true;
        if (!localMovie || peerMovie != localMovie) return true;
        if (peerVote == localMovie || forceSkip == localMovie) return true;
        return false;
    }
}
