Scriptname RumourEngine Hidden
{RumourEngine's Papyrus API. Global natives only: nothing is attached to anything, so nothing is stored in saves.
Rumours can be added and queried once a save's database is open; wait for the RumourEngine_Ready ModEvent, or check IsReady().
Without a compile-time dependency, send the ModEvent RumourEngine_AddRumour with the JSON request as its string instead.}

; 1: API version 1, frozen on 2026-11-12. Later versions only add to it.
int Function GetApiVersion() global native

; True once a save's database is open and its timeline settled.
bool Function IsReady() global native

; Add a rumour told by or about akOrigin (an actor, or any reference whose location is used).
; asKey, if given, makes a later call with the same source and key update this rumour. Returns its id, or 0.
int Function AddRumour(string asSource, string asText, ObjectReference akOrigin, float afNotability = 0.5, Actor[] akSubjects = None, string asKey = "") global native

; The same, starting in one mill by its id ("whiterun.commons"). Takes only strings and a number, so a console
; mod that adds cgf can call it:  cgf "RumourEngine.AddRumourInMill" "Console" "Heimskr has been preaching all night" "whiterun.commons" 0.6 ""
int Function AddRumourInMill(string asSource, string asText, string asMill, float afNotability = 0.5, string asKey = "") global native

; The full request as JSON, as the C API takes it.
int Function AddRumourJson(string asRequestJson) global native

bool Function RetireRumour(int aiId) global native
bool Function RetireRumourByKey(string asSource, string asKey) global native

; Up to aiMax rumours akActor has heard, as their mill tells them.
string[] Function GetRumoursFor(Actor akActor, int aiMax = 3) global native

; The ids of akActor's mills, the one they live in first.
string[] Function GetMillsFor(Actor akActor) global native

bool Function HasHeard(int aiId, Actor akActor) global native
