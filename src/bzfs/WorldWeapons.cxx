/* bzflag
 * Copyright (c) 1993-2017 Tim Riker
 *
 * This package is free software;  you can redistribute it and/or
 * modify it under the terms of the license found in the file
 * named COPYING that should have accompanied this file.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

// class-interface header
#include "WorldWeapons.h"

#include "WorldInfo.h"
// system headers
#include <vector>

// common-interface headers
#include "TimeKeeper.h"
#include "ShotUpdate.h"
#include "Protocol.h"
#include "Address.h"
#include "StateDatabase.h"
#include "bzfsAPI.h"

// bzfs specific headers
#include "bzfs.h"
#include "ShotManager.h"

uint32_t WorldWeapons::fireShot(FlagType* type, float lifetime, float *pos, float tilt, float direction, float shotSpeed, int *shotID, float delayTime, TeamColor teamColor, PlayerId targetPlayerID)
{
  if (!BZDB.isTrue(StateDatabase::BZDB_WEAPONS)) {
    return INVALID_SHOT_GUID;
  }

  void *buf, *bufStart = getDirectMessageBuffer();

  FiringInfo firingInfo;
  firingInfo.timeSent = (float)TimeKeeper::getCurrent().getSeconds();
  firingInfo.flagType = type;
  firingInfo.lifetime = lifetime;
  firingInfo.shot.player = ServerPlayer;
  memmove(firingInfo.shot.pos, pos, 3 * sizeof(float));

  if (shotSpeed < 0)
    shotSpeed = BZDB.eval(StateDatabase::BZDB_SHOTSPEED);
  const float tiltFactor = cosf(tilt);

  firingInfo.shot.vel[0] = shotSpeed * tiltFactor * cosf(direction);
  firingInfo.shot.vel[1] = shotSpeed * tiltFactor * sinf(direction);
  firingInfo.shot.vel[2] = shotSpeed * sinf(tilt);
  firingInfo.shot.dt = delayTime;
  firingInfo.shot.team = teamColor;

  if (shotID != NULL && shotID == 0) {
    *shotID = getNewWorldShotID();
    firingInfo.shot.id = *shotID;
  }
  else if (shotID == NULL) {
    firingInfo.shot.id = getNewWorldShotID();
  }
  else {
    firingInfo.shot.id = *shotID;
  }

  buf = firingInfo.pack(bufStart);

  broadcastMessage(MsgShotBegin, (char*)buf - (char*)bufStart, bufStart);

  uint32_t shotGUID = ShotManager.AddShot(firingInfo, ServerPlayer);

  // Target the gm, construct it, and send packet
  if (type->flagAbbv == "GM") {
    ShotManager.SetShotTarget(shotGUID, targetPlayerID);

    char packet[ShotUpdatePLen + PlayerIdPLen];
    buf = (void*)packet;
    buf = firingInfo.shot.pack(buf);
    buf = nboPackUByte(buf, targetPlayerID);

    broadcastMessage(MsgGMUpdate, sizeof(packet), packet);
  }

  bz_ServerShotFiredEventData_V1 event;
  event.guid = shotGUID;
  event.flagType = type->flagAbbv;
  event.lifetime = lifetime;
  event.pos[0] = pos[0];
  event.pos[1] = pos[1];
  event.pos[2] = pos[2];
  event.lookAt[0] = cosf(direction);
  event.lookAt[1] = sinf(direction);
  event.lookAt[2] = sinf(tilt);
  event.team = convertTeam(teamColor);

  WorldEventManager worldEventManager;
  worldEventManager.callEvents(bz_eServerShotFiredEvent, &event);

  return shotGUID;
}

WorldWeapons::WorldWeapons()
: worldShotId(0)
{
}


WorldWeapons::~WorldWeapons()
{
  clear();
}


void WorldWeapons::clear(void)
{
  for (std::vector<Weapon*>::iterator it = weapons.begin();
       it != weapons.end(); ++it) {
    Weapon *w = *it;
    delete w;
  }
  weapons.clear();
}


float WorldWeapons::nextTime ()
{
  TimeKeeper nextShot = TimeKeeper::getSunExplodeTime();
  for (std::vector<Weapon*>::iterator it = weapons.begin();
       it != weapons.end(); ++it) {
    Weapon *w = *it;
    if (w->nextTime <= nextShot) {
      nextShot = w->nextTime;
    }
  }
  return (float)(nextShot - TimeKeeper::getCurrent());
}


void WorldWeapons::fire()
{
  TimeKeeper nowTime = TimeKeeper::getCurrent();

  for (std::vector<Weapon*>::iterator it = weapons.begin();
       it != weapons.end(); ++it) {
    Weapon *w = *it;
    if (w->nextTime <= nowTime) {
      FlagType type = *(w->type);	// non-const copy

      fireShot(&type, BZDB.eval(StateDatabase::BZDB_RELOADTIME), w->origin, w->tilt, w->direction, w->direction, NULL, 0, w->teamColor);

      //Set up timer for next shot, and eat any shots that have been missed
      while (w->nextTime <= nowTime) {
	w->nextTime += w->delay[w->nextDelay];
	w->nextDelay++;
	if (w->nextDelay == (int)w->delay.size()) {
	  w->nextDelay = 0;
	}
      }
    }
  }
}


void WorldWeapons::add(const FlagType *type, const float *origin,
		       float direction, float tilt, TeamColor teamColor,
		       float initdelay, const std::vector<float> &delay,
		       TimeKeeper &sync)
{
  Weapon *w = new Weapon();
  w->type = type;
  w->teamColor = teamColor;
  memmove(&w->origin, origin, 3*sizeof(float));
  w->direction = direction;
  w->tilt = tilt;
  w->nextTime = sync;
  w->nextTime += initdelay;
  w->initDelay = initdelay;
  w->nextDelay = 0;
  w->delay = delay;

  weapons.push_back(w);
}


unsigned int WorldWeapons::count(void)
{
  return weapons.size();
}


void * WorldWeapons::pack(void *buf) const
{
  buf = nboPackUInt(buf, weapons.size());

  for (unsigned int i=0 ; i < weapons.size(); i++) {
    const Weapon *w = (const Weapon *) weapons[i];
    buf = w->type->pack (buf);
    buf = nboPackVector(buf, w->origin);
    buf = nboPackFloat(buf, w->direction);
    buf = nboPackFloat(buf, w->initDelay);
    buf = nboPackUShort(buf, (uint16_t)w->delay.size());
    for (unsigned int j = 0; j < w->delay.size(); j++) {
      buf = nboPackFloat(buf, w->delay[j]);
    }
  }
  return buf;
}


int WorldWeapons::packSize(void) const
{
  int fullSize = 0;

  fullSize += sizeof(uint32_t);

  for (unsigned int i=0 ; i < weapons.size(); i++) {
    const Weapon *w = (const Weapon *) weapons[i];
    fullSize += FlagType::packSize; // flag type
    fullSize += sizeof(float[3]); // pos
    fullSize += sizeof(float);    // direction
    fullSize += sizeof(float);    // init delay
    fullSize += sizeof(uint16_t); // delay count
    for (unsigned int j = 0; j < w->delay.size(); j++) {
      fullSize += sizeof(float);
    }
  }

  return fullSize;
}


int WorldWeapons::getNewWorldShotID(void)
{
  if (worldShotId > _MAX_WORLD_SHOTS) {
    worldShotId = 0;
  }
  return worldShotId++;
}

bool shotUsedInList(int shotID, Shots::ShotList& list)
{
	for (size_t s = 0; s < list.size(); s++)
	{
		if (list[s]->GetLocalShotID() == shotID)
			return true;
	}
	return false;
}

//----------WorldWeaponGlobalEventHandler---------------------
// where we do the world weapon handling for event based shots since they are not really done by the "world"

WorldWeaponGlobalEventHandler::WorldWeaponGlobalEventHandler(FlagType *_type,
							     const float *_origin,
							     float _direction,
							     float _tilt,
							     TeamColor teamColor )
{
  type = _type;
  if (_origin)
    memcpy(origin,_origin,sizeof(float)*3);
  else
    origin[0] = origin[1] = origin[2] = 0.0f;

  direction = _direction;
  tilt = _tilt;
  team = convertTeam(teamColor);
}

WorldWeaponGlobalEventHandler::~WorldWeaponGlobalEventHandler()
{
}

void WorldWeaponGlobalEventHandler::process (bz_EventData *eventData)
{
  if (!eventData || eventData->eventType != bz_eCaptureEvent)
    return;

  bz_CTFCaptureEventData_V1 *capEvent = (bz_CTFCaptureEventData_V1*)eventData;

  if (capEvent->teamCapped != team)
    return;

  world->getWorldWeapons().fireShot(type, BZDB.eval(StateDatabase::BZDB_RELOADTIME), origin, tilt, direction, -1, NULL, 0);
}

// Local Variables: ***
// mode: C++ ***
// tab-width: 8 ***
// c-basic-offset: 2 ***
// indent-tabs-mode: t ***
// End: ***
// ex: shiftwidth=2 tabstop=8
