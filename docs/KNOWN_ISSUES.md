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

## A side push from the partner's car can throw you off

**What you see:** your partner rams you from the side and your car is thrown hard, or off the road,
instead of being nudged.

**Why:** each game runs the physics for its own car only. The partner's car on your screen is moved by
the network, so to your physics it is a body that is forced to a place every frame: it pushes through
you with no mass of its own, and the push you get is not the one a real car of that weight would give.
The partner's game sees the mirror image of it.

**Better fix (not done yet):** a shared contact model - both games measure the hit, agree on one
impulse (by both cars' masses and speeds) and each applies its half to its own car, instead of the
forced position doing the pushing. Needs a careful two-PC test at low and high speed.

## Hitting a car the partner's game owns (their traffic, their cops)

**What you see (up to now):** at full speed into a traffic car or a cop car of the partner's game
(roadblocks above all) you stopped dead, as against a wall; the other car moved a moment later.

**Why:** that car is a copy on your screen, put at the owner's position every frame. Your hit was
sent to the owner, who pushes the real car, and the copy moved only when that came back.

**Now:** shortly before the contact your game's physics takes the copy over (the network stops moving
it), so you hit and push it as a normal car; after ~1.2 s (longer while you still touch it, 3 s at
most) it glides back to where the owner has it (`HitUtil::TakeLocalOnApproach`, `traffic.inl`
`TakeLocalHits`). The owner still gets the hit and pushes the real car, so both end up close.
Left: where the copy ends up on your screen and where the owner's push left the real car can differ
by a few meters - that is the glide you see. Race opponents of the partner are not taken over.

## The race start: a short drop at the end of the intro

**What you saw:** when the intro cutscene ended, both cars stood a bit above the road and dropped onto
it; about a second later the client's car jumped to its own grid slot.

**Now:** the host picks the client's slot on its first frame after the intro and the client moves there
as soon as its own intro is over and the slot has arrived (it no longer waits for the host's car to
show up on its spot). After a restart the cars are set down on the road too (seen 2 m above it).
Left: if the client's intro ends before the host's word arrives, the client stands on the player's
spot until it does (one network trip). A car you never drove in free roam (a Challenge Series car) has
no learned ride height: only a clear drop is cut, to a usual 0.75 m.
