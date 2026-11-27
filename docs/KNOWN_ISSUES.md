# Known issues

Problems that are understood but not fixed yet, with the cause and the better fix we have in mind.

## Traffic is only partly shared in Challenge Series tollbooths (and other race-type events)

**What you see:** in a tollbooth time trial (race type 4), some cars are on one screen only. Pursuit
challenges (race type 8) and free roam are not affected.

**Why:** the client treats every event with a race type other than 8 as a race
(`CoopRace::ClientTrafficAsRace`, `traffic.inl` `TrafficTick`). In a race the client keeps its own
traffic: no traffic car of its own is removed. With `[Traffic] SharedInRaces=1` it binds its own
traffic cars to the host's cars (`BindRaceTraffic`); a host car with no client car for it gets a copy
(out of view, with room), and near the host the client builds no new traffic of its own. That leaves
one gap: the client's own cars from before the race that are not bound drive on by themselves (never
sent to the host) until the game culls them.
Farther than 450 m from the host each player has their own traffic (as in free roam).

This was done on purpose for real races: race scripts own some traffic, and removing the client's
traffic crashed drag races (12.7.3, ~28 s in).

**Better fix (not done yet):** decide per event type instead of "every race type".
- Tollbooth (4) has no opponents and no scripted traffic. It could share traffic exactly like a pursuit
  challenge: return false from `ClientTrafficAsRace` for type 4 (the client then zeroes its density and
  spawns mirrors of the host's cars).
- That also suppresses the client's own patrol cops near the host. So the tollbooth must then also be
  an "open world" event for pursuits (`SharedPursuit::OpenWorldShared`, today only type 8):
  shared heat, the partner's chase pulls you in, and the partner's cops can spot you. Otherwise the client
  near the host has no cops of its own to start its pursuit.
- Keep the current binding for real races (sprint, circuit, drag, knockout, speedtrap). There, a
  smarter version would make the client's unbound cars leave once out of view (or bind them as soon as a
  host car appears) instead of driving on unsynced.
- Test on two PCs: 19.8.32 and 19.8.37 (tollbooths with cops), 12.7.3 (drag, the old crash), a sprint
  with traffic, and a pursuit challenge for regressions.
