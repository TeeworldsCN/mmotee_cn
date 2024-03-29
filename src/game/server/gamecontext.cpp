/* 2019年2月10日13:36:27 */
/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <new>
#include <base/math.h>
#include <engine/shared/config.h>
#include <engine/map.h>
#include <engine/console.h>
#include <engine/storage.h>
#include <engine/server/sql_server.h>
#include "gamecontext.h"
#include <game/version.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <iostream>
#include "gamemodes/mod.h"  
#include <game/server/entities/loltext.h>
#define BOSSID 62
const int MAX_COUNT = 1e9;

enum
{
	RESET,
	NO_RESET
};

void CGameContext::OnSetAuthed(int ClientID, int Level)
{
	if(m_apPlayers[ClientID])
		m_apPlayers[ClientID]->m_Authed = Level;
}

void CGameContext::Construct(int Resetting)
{
	m_Resetting = 0;
	m_pServer = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_apPlayers[i] = 0;
	}
	m_pController = 0;

	// боссецкий чистка
	m_BossStart = false;
	m_BossType = 0;
	m_BossStartTick = 0;
	m_WinWaitBoss = 0;
	m_ChatResponseTargetID = -1;
}

CGameContext::CGameContext(int Resetting)
{
	Construct(Resetting);
}

CGameContext::CGameContext()
{
	Construct(NO_RESET);
}

CGameContext::~CGameContext()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		delete m_apPlayers[i];
	}
}

void CGameContext::ClearVotes(int ClientID)
{
	m_PlayerVotes[ClientID].clear();
	
	// send vote options
	CNetMsg_Sv_VoteClearOptions ClearMsg;
	Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::Clear()
{	
	CTuningParams Tuning = m_Tuning;

	m_Resetting = true;
	this->~CGameContext();
	mem_zero(this, sizeof(*this));
	new (this) CGameContext(RESET);

	m_Tuning = Tuning;
	
	for(int i=0; i<MAX_CLIENTS; i++)
	{
		m_BroadcastStates[i].m_NoChangeTick = 0;
		m_BroadcastStates[i].m_LifeSpanTick = 0;
		m_BroadcastStates[i].m_Priority = BROADCAST_PRIORITY_LOWEST;
		m_BroadcastStates[i].m_PrevMessage[0] = 0;
		m_BroadcastStates[i].m_NextMessage[0] = 0;
	}
} 

class CCharacter *CGameContext::GetPlayerChar(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return 0;

	return m_apPlayers[ClientID]->GetCharacter();
}

void CGameContext::CreateDamageInd(vec2 Pos, float Angle, int Amount)
{
	float a = 3 * 3.14159f / 2 + Angle;
	float s = a-pi/3;
	float e = a+pi/3;
	for(int i = 0; i < Amount; i++)
	{
		float f = mix(s, e, float(i+1)/float(Amount+2));
		CNetEvent_DamageInd *pEvent = (CNetEvent_DamageInd *)m_Events.Create(NETEVENTTYPE_DAMAGEIND, sizeof(CNetEvent_DamageInd));
		if(pEvent)
		{
			pEvent->m_X = (int)Pos.x;
			pEvent->m_Y = (int)Pos.y;
			pEvent->m_Angle = (int)(f*256.0f);
		}
	}
}

void CGameContext::CreateHammerHit(vec2 Pos)
{
	// create the event
	CNetEvent_HammerHit *pEvent = (CNetEvent_HammerHit *)m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateExplosion(vec2 Pos, int Owner, int Weapon, bool NoDamage, int TakeDamageMode)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}

	if (!NoDamage)
	{
		// deal damage
		CCharacter *apEnts[MAX_CLIENTS];
		float Radius = 135.0f;
		float InnerRadius = 48.0f;
		int Num = m_World.FindEntities(Pos, Radius, (CEntity**)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		for(int i = 0; i < Num; i++)
		{
			if(apEnts[i])
			{
				vec2 Diff = apEnts[i]->m_Pos - Pos;
				vec2 ForceDir(0,1);
				float l = length(Diff);
				if(l)
					ForceDir = normalize(Diff);
				l = 1-clamp((l-InnerRadius)/(Radius-InnerRadius), 0.0f, 1.0f);
				float Dmg = 6 * l;
				if((int)Dmg)
				{
					if(m_apPlayers[Owner] && m_apPlayers[Owner]->GetCharacter() && m_apPlayers[Owner]->GetCharacter()->m_ActiveWeapon == WEAPON_SHOTGUN)	
						Dmg = (int)(Dmg/60);	
						
					apEnts[i]->TakeDamage(ForceDir*Dmg*2, (int)Dmg , Owner, Weapon, TakeDamageMode);
				}
			}
		}
	}
}

// Thanks to Stitch for the idea
void CGameContext::CreateExplosionDisk(vec2 Pos, float InnerRadius, float DamageRadius, int Damage, float Force, int Owner, int Weapon, int TakeDamageMode)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}
	if(Damage > 0)
	{
		// deal damage
		CCharacter *apEnts[MAX_CLIENTS];
		int Num = m_World.FindEntities(Pos, DamageRadius, (CEntity**)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		for(int i = 0; i < Num; i++)
		{
			vec2 Diff = apEnts[i]->m_Pos - Pos;
			vec2 ForceDir(0,1);
			float l = length(Diff);
			l = 1-clamp((l-InnerRadius)/(DamageRadius-InnerRadius), 0.0f, 1.0f);
			
			if(l)
				ForceDir = normalize(Diff);
			
			float DamageToDeal = 1 + ((Damage - 1) * l);
			apEnts[i]->TakeDamage(ForceDir*Force*l, DamageToDeal, Owner, Weapon, TakeDamageMode);
		}
	}
	
	float CircleLength = 2.0*pi*max(DamageRadius-135.0f, 0.0f);
	int NumSuroundingExplosions = CircleLength/32.0f;
	float AngleStart = random_float()*pi*2.0f;
	float AngleStep = pi*2.0f/static_cast<float>(NumSuroundingExplosions);
	for(int i=0; i<NumSuroundingExplosions; i++)
	{
		CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
		if(pEvent)
		{
			pEvent->m_X = (int)Pos.x + (DamageRadius-135.0f) * cos(AngleStart + i*AngleStep);
			pEvent->m_Y = (int)Pos.y + (DamageRadius-135.0f) * sin(AngleStart + i*AngleStep);
		}
	}
}

void CGameContext::CreatePlayerSpawn(vec2 Pos)
{
	// create the event
	CNetEvent_Spawn *ev = (CNetEvent_Spawn *)m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn));
	if(ev)
	{
		ev->m_X = (int)Pos.x;
		ev->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateDeath(vec2 Pos, int ClientID)
{
	// create the event
	CNetEvent_Death *pEvent = (CNetEvent_Death *)m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_ClientID = ClientID;
	}
}

void CGameContext::CreateSound(vec2 Pos, int Sound, int64_t Mask)
{
	if (Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_SoundID = Sound;
	}
}

void CGameContext::CreateSoundGlobal(int Sound, int Target)
{
	if (Sound < 0)
		return;

	CNetMsg_Sv_SoundGlobal Msg;
	Msg.m_SoundID = Sound;
	if(Target == -2)
		Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
	else
	{
		int Flag = MSGFLAG_VITAL;
		if(Target != -1)
			Flag |= MSGFLAG_NORECORD;
		Server()->SendPackMsg(&Msg, Flag, Target);
	}
}

void CGameContext::SendChatTarget(int To, const char *pText)
{
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientID = -1;
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
}

void CGameContext::SendChatTarget_Localization(int To, int Category, const char* pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_CLIENTS : To+1);
	
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientID = -1;
	
	dynamic_string Buffer;
	
	va_list VarArgs;
	va_start(VarArgs, pText);
	
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			Buffer.clear();
			switch(Category)
			{
				case CHATCATEGORY_ASSASINS:
					Buffer.append("₪ | ");
					break;
				case CHATCATEGORY_BERSERK:
					Buffer.append("❖ | ");
					break;
				case CHATCATEGORY_NOAUTH:
					Buffer.append("♟ | ");
					break;
				case CHATCATEGORY_HEALER:
					Buffer.append("ღ | ");
					break;
				case CHATCATEGORY_ACCUSATION:
					Buffer.append("☹ | ");
					break;
			}
			Server()->Localization()->Format_VL(Buffer, m_apPlayers[i]->GetLanguage(), pText, VarArgs);
			
			Msg.m_pMessage = Buffer.buffer();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
		}
	}
	
	Buffer.clear();
	va_end(VarArgs);
}


void CGameContext::AddVote_Localization(int To, const char* aCmd, const char* pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_NOBOT : To+1);
	
	dynamic_string Buffer;
	
	va_list VarArgs;
	va_start(VarArgs, pText);
	
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			Buffer.clear();
			Server()->Localization()->Format_VL(Buffer, m_apPlayers[i]->GetLanguage(), pText, VarArgs);
			AddVote(Buffer.buffer(), aCmd, i);
		}
	}
	
	Buffer.clear();
	va_end(VarArgs);
}

void CGameContext::AddVoteMenu_Localization(int To, int MenuID, int Type, const char* pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_NOBOT : To+1);
	
	dynamic_string Buffer;
	
	va_list VarArgs;
	va_start(VarArgs, pText);
	
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			Buffer.clear();
			Server()->Localization()->Format_VL(Buffer, m_apPlayers[i]->GetLanguage(), pText, VarArgs);

			if(Type == MENUONLY)
			{
				char Buf[8];
				str_format(Buf, sizeof(Buf), "menu%d", MenuID);
				AddVote(Buffer.buffer(), Buf, i);
			}
			else if(Type == SETTINGSONLY)
			{
				char Buf[8];
				str_format(Buf, sizeof(Buf), "set%d", MenuID);
				AddVote(Buffer.buffer(), Buf, i);					
			}
			else if(Type == BUYITEMONLY)
			{
				char Buf[8];
				str_format(Buf, sizeof(Buf), "bon%d", MenuID);
				AddVote(Buffer.buffer(), Buf, i);				
			}
			else if(Type == SELLITEMWORK)
			{
				char Buf[8];
				str_format(Buf, sizeof(Buf), "seli%d", MenuID);
				AddVote(Buffer.buffer(), Buf, i);				
			}
			else if(Type == CRAFTONLY)
			{
				char Buf[8];
				str_format(Buf, sizeof(Buf), "cra%d", MenuID);
				AddVote(Buffer.buffer(), Buf, i);				
			}
		}
	}
	
	Buffer.clear();
	va_end(VarArgs);
}

void CGameContext::SendChatTarget_Localization_P(int To, int Category, int Number, const char* pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_NOBOT : To+1);
	
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientID = -1;
	
	dynamic_string Buffer;
	
	va_list VarArgs;
	va_start(VarArgs, pText);
	
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			Buffer.clear();
			switch(Category)
			{
				case CHATCATEGORY_ACCUSATION:
					Buffer.append("☹ | ");
					break;
			}
			Server()->Localization()->Format_VLP(Buffer, m_apPlayers[i]->GetLanguage(), Number, pText, VarArgs);
			
			Msg.m_pMessage = Buffer.buffer();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
		}
	}
	
	Buffer.clear();
	va_end(VarArgs);
}

void CGameContext::SendMOTD(int To, const char* pText)
{
	if(m_apPlayers[To])
	{
		CNetMsg_Sv_Motd Msg;
		
		Msg.m_pMessage = pText;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
	}
}

void CGameContext::SendMOTD_Localization(int To, const char* pText, ...)
{
	if(m_apPlayers[To])
	{
		dynamic_string Buffer;
		
		CNetMsg_Sv_Motd Msg;
		
		va_list VarArgs;
		va_start(VarArgs, pText);
		
		Server()->Localization()->Format_VL(Buffer, m_apPlayers[To]->GetLanguage(), pText, VarArgs);
	
		va_end(VarArgs);
		
		Msg.m_pMessage = Buffer.buffer();
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
		Buffer.clear();
	}
}

void CGameContext::SendGuide(int ClientID, int BossType)
{
	if(!m_apPlayers[ClientID])
		return;
	
	int arghealth = 0;
	const char* argtext = "null";
	const char* pLanguage = m_apPlayers[ClientID]->GetLanguage();
	
	dynamic_string Buffer;		
	if(BossType == BOT_BOSSSLIME)
	{
		if(!g_Config.m_SvCityStart)
		{
			arghealth = 500;
			argtext = "Healer";
			Server()->Localization()->Format_L(Buffer, pLanguage, _("Weapon: Shotgun / Speed: Normal+\nRank Boss D\n\nHeadshot's: if you are near!\n\nReward:\n- Money bag x3-8\n- 5% - craft item\n- 20% - Random Craft Box"), NULL);
		}
		else if(g_Config.m_SvCityStart == 1)
		{
			arghealth = 1000;
			argtext = "All";
			Server()->Localization()->Format_L(Buffer, pLanguage, _("Weapon: Shotgun / Speed: Fast+\nRank Boss B\n\nHeadshot's: if you are near!\n\nReward:\n- Money bag x40-80\n- 5% - craft item\n- 20% - Random Craft Box"), NULL);
		}
	}
	int Time = m_BossStartTick/Server()->TickSpeed();
	SendMOTD_Localization(ClientID, "Guide / Boss: {str:name}\n生命值加上等级*{int:hp}\nRecomended class: {str:rclass}\n\n{str:guide}\n\n\n等待玩家进入Boss战，还剩下 {int:siska} 秒.", 
		"name", GetBossName(m_BossType), "hp", &arghealth, "rclass", argtext, "guide", Buffer.buffer(), "siska", &Time);
		
	Buffer.clear();
}

void CGameContext::AddBroadcast(int ClientID, const char* pText, int Priority, int LifeSpan)
{
	if(LifeSpan > 0)
	{
		if(m_BroadcastStates[ClientID].m_TimedPriority > Priority)
			return;
			
		str_copy(m_BroadcastStates[ClientID].m_TimedMessage, pText, sizeof(m_BroadcastStates[ClientID].m_TimedMessage));
		m_BroadcastStates[ClientID].m_LifeSpanTick = LifeSpan;
		m_BroadcastStates[ClientID].m_TimedPriority = Priority;
	}
	else
	{
		if(m_BroadcastStates[ClientID].m_Priority > Priority)
			return;
			
		str_copy(m_BroadcastStates[ClientID].m_NextMessage, pText, sizeof(m_BroadcastStates[ClientID].m_NextMessage));
		m_BroadcastStates[ClientID].m_Priority = Priority;
	}
}

void CGameContext::SendBroadcast(int To, const char *pText, int Priority, int LifeSpan)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_NOBOT : To+1);
	
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
			AddBroadcast(i, pText, Priority, LifeSpan);
	}
}

void CGameContext::CreateLolText(CEntity *pParent, bool Follow, vec2 Pos, vec2 Vel, int Lifespan, const char *pText)
{
	CLoltext::Create(&m_World, pParent, Pos, Vel, Lifespan, pText, true, Follow);
}

void CGameContext::ClearBroadcast(int To, int Priority)
{
	SendBroadcast(To, "", Priority, BROADCAST_DURATION_REALTIME);
}

void CGameContext::SendBroadcast_Localization(int To, int Priority, int LifeSpan, const char* pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_NOBOT : To+1);
	
	dynamic_string Buffer;
	
	va_list VarArgs;
	va_start(VarArgs, pText);
	
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			Buffer.clear();
			Server()->Localization()->Format_VL(Buffer, m_apPlayers[i]->GetLanguage(), pText, VarArgs);
			AddBroadcast(i, Buffer.buffer(), Priority, LifeSpan);
		}
	}
	
	Buffer.clear();
	va_end(VarArgs);
}

void CGameContext::SendBroadcast_LStat(int To, int Priority, int LifeSpan, int Type, int Size, int Size2)
{
	if(!m_apPlayers[To])
		return;
		
	if(!m_apPlayers[To]->GetCharacter() || m_apPlayers[To]->IsBot())
		return;

	int Optmem = m_apPlayers[To]->AccData.Level*m_apPlayers[To]->GetNeedForUp();
	float getlv = (m_apPlayers[To]->AccData.Exp*100.0)/Optmem;
	int getl = (int)getlv;
	const char *Level = LevelString(100, (int)getlv, 10, '*', ' ');
	
	getlv = (m_apPlayers[To]->m_AngryWroth*100.0)/250;
	const char *Angry = LevelString(100, (int)getlv, 10, '*', ' ');

	getlv = (m_apPlayers[To]->m_Mana*100.0)/m_apPlayers[To]->GetNeedMana();
	const char *Mana = LevelString(100, (int)getlv, 5, ':', ' ');

	const char* pLanguage = m_apPlayers[To]->GetLanguage();
	dynamic_string Buffer;
	switch(Type)
	{
		case INSHOP: 
			Server()->Localization()->Format_L(Buffer, pLanguage, _("你好{str:name}(*′▽｀)ノノ! 你现在在商店，商店菜单现在可用了"), "name", Server()->ClientName(To), NULL), Buffer.append("\n");
			break;
		case EXITSHOP: 
			Server()->Localization()->Format_L(Buffer, pLanguage, _("再见，我的{str:name}!"), "name", Server()->ClientName(To), NULL), Buffer.append("\n");
			break;
		case INADDMONEY:  
			Server()->Localization()->Format_L(Buffer, pLanguage, _("白银 +{int:point}"), "point", &Size, NULL), Buffer.append("\n");
			break;
		case INADDEXP:  
			Server()->Localization()->Format_L(Buffer, pLanguage, _("经验 +{int:point}"), "point", &Size, NULL), Buffer.append("\n");
			break;
		case NOTWEAPON:  
			Server()->Localization()->Format_L(Buffer, pLanguage, _("你需要购买该武器才能拾取该武器的弹药!"), NULL), Buffer.append("\n");
			break;
		case INADDCEXP:  
		{
			Server()->Localization()->Format_L(Buffer, pLanguage, _("经验 +{int:point}. 公会 +{int:struct}"), "point", &Size, "struct", &Size2, NULL), Buffer.append("\n");
		}	break;
		case INANTIPVP:
			Server()->Localization()->Format_L(Buffer, pLanguage, _("这里是禁止PVP的区域."), NULL),	Buffer.append("\n");
			break; 
		case EXITANTIPVP:
			Server()->Localization()->Format_L(Buffer, pLanguage, _("这里是允许PVP的区域."), NULL),	Buffer.append("\n"); 
			break; 
		case INCRAFT:
			Server()->Localization()->Format_L(Buffer, pLanguage, _("你好{str:name}(*′▽｀)ノノ! 你现在在合成室，合成菜单现在可用了."), "name", Server()->ClientName(To), NULL),	Buffer.append("\n"); 
			break; 
		case INQUEST:
			Server()->Localization()->Format_L(Buffer, pLanguage, _("你好{str:name}(*′▽｀)ノノ! 你现在在任务大厅，任务菜单可用了."), "name", Server()->ClientName(To), NULL),	Buffer.append("\n"); 
			break; 
		default: Buffer.clear();
	}
		
	SendBroadcast_Localization(To, Priority, LifeSpan, " \n\n等级: {int:lvl} | 经验: {int:exp}/{int:expl}\n----------------------\n{str:sdata} {int:getl}%\n{str:dataang} 怒气\n----------------------\n{str:mana} 魔能\n生命值: {int:hp}/{int:hpstart}\n\n\n\n\n\n\n\n\n\n\n\n{str:buff}{str:emp}", 
		"lvl", &m_apPlayers[To]->AccData.Level, "exp", &m_apPlayers[To]->AccData.Exp, "expl", &Optmem, "sdata", Level, "getl", &getl, "dataang", Angry, "mana", Mana, "hp", &m_apPlayers[To]->m_Health, "hpstart", &m_apPlayers[To]->m_HealthStart, "buff", Buffer.buffer(),
		"emp", "                                                                                                                                                                    ");

	delete Level;
	delete Angry;
	delete Mana;
	Buffer.clear();
}

void CGameContext::SendBroadcast_LChair(int To, int SizeExp, int SizeMoney)
{
	if(!m_apPlayers[To] || !m_apPlayers[To]->GetCharacter() || m_apPlayers[To]->IsBot())
		return;
				
	int Optmem = m_apPlayers[To]->AccData.Level*m_apPlayers[To]->GetNeedForUp();
	int Optexp = m_apPlayers[To]->AccData.Exp;
	
	float getlv = (Optexp*100.0)/Optmem;
	int getl = (int)getlv;
	const char *aBuf = LevelString(100, (int)getlv, 5, '*', ' ');
	
	dynamic_string Buffer;		
	SendBroadcast_Localization(To, 105, 100, "{str:buff}{str:sdata}({int:getl}%)\n经验: {int:exp}/{int:expl} +{int:exps}\n白银: {int:money} +{int:moneys}", 
		"buff", Buffer.buffer(), "sdata", aBuf, "getl", &getl, "exp", &m_apPlayers[To]->AccData.Exp, "expl", &Optmem, "exps", &SizeExp, "money", &m_apPlayers[To]->AccData.Money, "moneys", &SizeMoney);

	delete aBuf;
	Buffer.clear();
	return;
}

void CGameContext::SendBroadcast_LBossed(int To, int Priority, int LifeSpan)
{
	if(!m_apPlayers[To] || !m_apPlayers[BOSSID])
		return;
		
	if(!m_apPlayers[To]->GetCharacter() || m_apPlayers[To]->IsBot())
		return;
		
	if(m_apPlayers[BOSSID]->GetCharacter())
	{
		int Optmem = m_apPlayers[BOSSID]->m_HealthStart;
		int Optexp = m_apPlayers[BOSSID]->m_Health;
		
		float getlv = (Optexp*100.0)/Optmem;
		int getl = (int)getlv;
		const char *aBuf = LevelString(100, (int)getlv, 5, ':', ' ');
			
		SendBroadcast_Localization(To, Priority, LifeSpan, "{str:sdata}\n({int:getl}%)\nBoss: {str:name} 生命值: {int:hp}/{int:hpstart}\n我的生命值 {int:yhp}/{int:yhps}", 
			"sdata", aBuf, "getl", &getl, "name", GetBossName(m_BossType), "hp", &Optexp, "hpstart", &Optmem, "yhp", &m_apPlayers[To]->m_Health, "yhps", &m_apPlayers[To]->m_HealthStart ,NULL);
	
		delete aBuf;
	}
	else
		SendBroadcast_Localization(To, Priority, LifeSpan, "太棒了! 玩家最终取得了胜利! Boss {str:name} 已经倒下!", "name", GetBossName(m_BossType), NULL);
}

void CGameContext::SendBroadcast_Localization_P(int To, int Priority, int LifeSpan, int Number, const char* pText, ...)
{
	int Start = (To < 0 ? 0 : To);
	int End = (To < 0 ? MAX_NOBOT : To+1);
	
	dynamic_string Buffer;
	
	va_list VarArgs;
	va_start(VarArgs, pText);
	
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			Server()->Localization()->Format_VLP(Buffer, m_apPlayers[i]->GetLanguage(), Number, pText, VarArgs);
			AddBroadcast(i, Buffer.buffer(), Priority, LifeSpan);
		}
	}
	
	va_end(VarArgs);
}

void CGameContext::SendChat(int ChatterClientID, int Team, const char *pText)
{
	if(Team == CGameContext::CHAT_ALL)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 0;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = pText;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
	}
	else
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 1;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = pText;

		// pack one for the recording only
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NOSEND, -1);

		// send to the clients
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_apPlayers[i])
			{
				int PlayerTeam = CGameContext::CHAT_BLUE;
				if(m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS) PlayerTeam = CGameContext::CHAT_SPEC;
				
				if(PlayerTeam == Team)
				{
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, i);
				}
			}
		}
	}
}

void CGameContext::SendEmoticon(int ClientID, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientID, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendTuningParams(int ClientID)
{
	CheckPureTuning();
	
	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = (int *)&m_Tuning;
	for(unsigned i = 0; i < sizeof(m_Tuning)/sizeof(int); i++)
		Msg.AddInt(pParams[i]);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::BossTick()
{
	// таймер ожидания победы распределения дропа
	if(m_WinWaitBoss)
	{
		m_WinWaitBoss--;
		if(m_WinWaitBoss == 2)
		{
			m_BossStart = false;
			m_BossStartTick = 0;
			m_WinWaitBoss = 0;
			m_BossType = 0;
			
			for(int i = 0; i < MAX_NOBOT; ++i)
			{
				if(m_apPlayers[i])
				{
					if(m_apPlayers[i]->m_InBossed)
					{
						m_apPlayers[i]->m_InBossed = false;
						if(m_apPlayers[i]->GetCharacter())
							m_apPlayers[i]->GetCharacter()->Die(i, WEAPON_WORLD);
					}
				}
			}	
		}
		if(m_WinWaitBoss == 950)
			DeleteBotBoss();	
	}
	
	// таймер ожидания игроков для рейда
	if(m_BossStartTick)
	{		
		m_BossStartTick--;
		if(!GetBossCount())
		{
			m_BossStartTick = 0;
			m_BossStart = false;
			SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("Boss战被取消了, 玩家们不在等待室内(waiting room)"), NULL);	
		}
		
		if(m_BossStartTick == 50)
		{			
			m_BossStart = true;
			for(int i = 0; i < MAX_NOBOT; ++i)
			{
				if(m_apPlayers[i])
				{
					if(m_apPlayers[i]->m_InBossed && m_apPlayers[i]->GetCharacter())
						m_apPlayers[i]->GetCharacter()->Die(i, WEAPON_WORLD);
				}
			}
			int CountBoss = GetBossCount();
			SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("对Boss战开始了. 玩家数量:{int:num}"), "num", &CountBoss, NULL);	
		}
	}

	// если комната босса активна и игроков меньше или равно 0, выдаем что босс проебан
	if(m_BossStart && !GetBossCount() && !m_WinWaitBoss)
	{
		m_BossStartTick = 0;
		m_BossStart = false;
		
		SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("Boss战的玩家都死亡了, boss {str:name} 最终胜利了"), "name", GetBossName(m_BossType), NULL);
		
		m_BossType = 0;
		DeleteBotBoss();
	}
}

void CGameContext::SendMail(int ClientID, const char* pText, int ItemID, int ItemNum)
{
	if(!Server()->IsClientLogged(ClientID))
		return;
	
	Server()->SendMail(Server()->GetUserID(ClientID), pText, ItemID, ItemNum);
	SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("您的邮箱内有新的信息"), NULL);
}

void CGameContext::AreaTick()
{
	// Старт игры сюрвиал
	if(m_AreaStartTick)
	{	
		m_AreaStartTick--;
		if(m_AreaStartTick == 10)
		{
			int count = GetAreaCount();
			if(count < 2)
			{
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("[Survial] 最少需要两名玩家才能开始"), NULL);	
				for(int i = 0; i < MAX_NOBOT; ++i)
				{
					if(m_apPlayers[i] && m_apPlayers[i]->m_InArea)
					{
						m_apPlayers[i]->m_InArea = false;				
						if(m_apPlayers[i]->GetCharacter())
							m_apPlayers[i]->GetCharacter()->Die(i, WEAPON_WORLD);					
					}
				}				
			}
			else
			{
				m_AreaEndGame = 120*Server()->TickSpeed();
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("[Survial] 事件开始了, 一共有 {int:num} 名玩家参加"), "num", &count, NULL);
			}	
		}
	}
	// Arena уже игра да пидоры
	if(m_AreaEndGame)
	{
		m_AreaEndGame--;
		if(m_AreaEndGame && !GetAreaCount())
			m_AreaEndGame = 0;
		
		if(!m_AreaEndGame)
		{
			for(int i = 0; i < MAX_NOBOT; ++i)
			{
				if(m_apPlayers[i] && m_apPlayers[i]->m_InArea)
				{
					m_apPlayers[i]->m_InArea = false;
					if(m_apPlayers[i]->GetCharacter())
						m_apPlayers[i]->GetCharacter()->Die(i, WEAPON_WORLD);
				}
			}		
			SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("[Survial] 事件结束了. 没有获胜的玩家"), NULL);		
		}

		if(GetAreaCount() == 1)
		{
			int is = 0;
			for(int i = 0; i < MAX_NOBOT; ++i)
			{
				if(m_apPlayers[i] && m_apPlayers[i]->m_InArea)
				{
					m_apPlayers[i]->AccData.WinArea++, is = i;
					if(m_apPlayers[i]->GetCharacter())
					{
						m_apPlayers[i]->GetCharacter()->Die(i, WEAPON_WORLD);
						
						switch(m_AreaType)
						{
							case 1: 
							{
								GiveItem(i, MONEYBAG, 10);
								
								int RandGet = rand()%2;
								if(RandGet == 1)
									SendMail(i, "你获得了奖品，真幸运!", RELRINGS, 1);						
							} break;
							case 2: 
							{
								GiveItem(i, MONEYBAG, 10);
								
								int RandGet = rand()%20;
								if(RandGet == 1)
									SendMail(i, "你获得了奖品，真幸运!", FREEAZER, 1);	
							} break;
						}					
					}
				}
			}
			SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("[Survial] 最终赢家是:{str:name}:"), "name", Server()->ClientName(is), NULL);	
			m_AreaEndGame = 0;
		}
	}
	// старт арены 
	if(Server()->Tick() % (1 * Server()->TickSpeed() * 600) == 0)
		StartArea(120, rand()%2+1);
}

void CGameContext::OnTick()
{
	// copy tuning
	m_World.m_Core.m_Tuning = m_Tuning;
	m_World.Tick();

	//if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	int NumActivePlayers = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			if(m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
				NumActivePlayers++;

			m_apPlayers[i]->Tick();
			m_apPlayers[i]->PostTick();
			
			if(m_InviteTick[i] > 0)
			{
				if(m_InviteTick[i] == 1)
				{
					m_InviteTick[i] = 0;
					
					CNetMsg_Sv_VoteSet Msg;
					Msg.m_Timeout = 0;
					Msg.m_pDescription = "";
					Msg.m_pReason = "";
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);	
				}
				else
				{
					m_InviteTick[i]--;
				}
			}
		}
	}
	
	//Check for new broadcast
	for(int i=0; i<MAX_NOBOT; i++)
	{
		if(m_apPlayers[i])
		{
			if(m_BroadcastStates[i].m_LifeSpanTick > 0 && m_BroadcastStates[i].m_TimedPriority > m_BroadcastStates[i].m_Priority)
			{
				str_copy(m_BroadcastStates[i].m_NextMessage, m_BroadcastStates[i].m_TimedMessage, sizeof(m_BroadcastStates[i].m_NextMessage));
			}
			
			//Send broadcast only if the message is different, or to fight auto-fading
			if(
				str_comp(m_BroadcastStates[i].m_PrevMessage, m_BroadcastStates[i].m_NextMessage) != 0 ||
				m_BroadcastStates[i].m_NoChangeTick > Server()->TickSpeed()
			)
			{
				CNetMsg_Sv_Broadcast Msg;
				Msg.m_pMessage = m_BroadcastStates[i].m_NextMessage;
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
				
				str_copy(m_BroadcastStates[i].m_PrevMessage, m_BroadcastStates[i].m_NextMessage, sizeof(m_BroadcastStates[i].m_PrevMessage));
				
				m_BroadcastStates[i].m_NoChangeTick = 0;
			}
			else
				m_BroadcastStates[i].m_NoChangeTick++;
			
			//Update broadcast state
			if(m_BroadcastStates[i].m_LifeSpanTick > 0)
				m_BroadcastStates[i].m_LifeSpanTick--;
			
			if(m_BroadcastStates[i].m_LifeSpanTick <= 0)
			{
				m_BroadcastStates[i].m_TimedMessage[0] = 0;
				m_BroadcastStates[i].m_TimedPriority = BROADCAST_PRIORITY_LOWEST;
			}
			m_BroadcastStates[i].m_NextMessage[0] = 0;
			m_BroadcastStates[i].m_Priority = BROADCAST_PRIORITY_LOWEST;
		}
		else
		{
			m_BroadcastStates[i].m_NoChangeTick = 0;
			m_BroadcastStates[i].m_LifeSpanTick = 0;
			m_BroadcastStates[i].m_Priority = BROADCAST_PRIORITY_LOWEST;
			m_BroadcastStates[i].m_TimedPriority = BROADCAST_PRIORITY_LOWEST;
			m_BroadcastStates[i].m_PrevMessage[0] = 0;
			m_BroadcastStates[i].m_NextMessage[0] = 0;
			m_BroadcastStates[i].m_TimedMessage[0] = 0;
		}
	}

	if(Server()->Tick() % (1 * Server()->TickSpeed() * 520) == 0)
	{
		Server()->GetTopClanHouse();
	}

	if(Server()->Tick() % (1 * Server()->TickSpeed() * 610) == 0)
	{
		SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("### 服务器信息:"), NULL);	
		SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("大家注意了!! 新人必须遵守的规则:"), NULL);	
		SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("只能玩一个服务器, 如果玩了 1-250 的服务器之后才能玩 250-500 的服务器"), NULL);	
		SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("你的账户如果被删除,你创建的公会会重组(reform)."), NULL);	
		SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("服务器版本: 1.1 Kurosio测试通过,中文翻译:MC_TYH以及全体MMOTEE国服玩家."), NULL);	
	}

	// вывод топ листа раз в 5 минут
	if(Server()->Tick() % (1 * Server()->TickSpeed() * 440) == 0)
	{
		switch(rand()%7)
		{
			case 0:
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("(* ^ ω ^) 玩家排行榜前五名:{str:name}:"), "name", "等级", NULL);	
				Server()->ShowTop10(25, "Level", 2); break;
			case 1:
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("(* ^ ω ^) 玩家排行榜前五名:{str:name}:"), "name", "黄金", NULL);	
				Server()->ShowTop10(25, "Gold", 2); break;
			case 2:
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("(* ^ ω ^) 玩家排行榜前五名:{str:name}:"), "name", "Win Area", NULL);	
				Server()->ShowTop10(25, "WinArea", 2); break;		
			case 3:
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("(* ^ ω ^) 玩家排行榜前五名:{str:name}:"), "name", "击杀", NULL);	
				Server()->ShowTop10(25, "Killing", 2); break;
			case 4:
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("(* ^ ω ^) 公会排行榜前五名:{str:name}:"), "name", "等级", NULL);	
				Server()->ShowTop10Clans(25, "Level", 2); break;
			case 5:
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("(* ^ ω ^) 公会排行榜前五名:{str:name}:"), "name", "Relevance", NULL);	
				Server()->ShowTop10Clans(25, "Relevance", 2); break;
			default:
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("(* ^ ω ^) 公会排行榜前五名:{str:name}:"), "name", "白银", NULL);	
				Server()->ShowTop10Clans(25, "Money", 2); break;
		}
	}

	AreaTick();
	BossTick();

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies ; i++)
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i&1)?-1:1;
			m_apPlayers[MAX_CLIENTS-i-1]->OnPredictedInput(&Input);
		}
	}
#endif
}

// Server hooks
void CGameContext::OnClientDirectInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnDirectInput((CNetObj_PlayerInput *)pInput);
}

void CGameContext::OnClientPredictedInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnPredictedInput((CNetObj_PlayerInput *)pInput);
}

void CGameContext::OnClientEnter(int ClientID)
{
	//world.insert_entity(&players[client_id]);
	m_apPlayers[ClientID]->m_IsInGame = true;
	m_apPlayers[ClientID]->Respawn();
	
	ResetVotes(ClientID, NOAUTH);
	Server()->FirstInit(ClientID);
	
	SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("玩家 {str:PlayerName} 进入了服务器!"), "PlayerName", Server()->ClientName(ClientID), NULL);
	SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("欢迎! 请登录(/login)或者注册(/register)一个新的账户,指令见/cmdlist"), NULL);
}

void CGameContext::OnClientConnected(int ClientID)
{
	const int StartTeam = g_Config.m_SvTournamentMode ? TEAM_SPECTATORS : m_pController->GetAutoTeam(ClientID);
	
	if(!m_apPlayers[ClientID])
		m_apPlayers[ClientID] = new(ClientID) CPlayer(this, ClientID, StartTeam);
	else
	{
		delete m_apPlayers[ClientID];
		m_apPlayers[ClientID] = new(ClientID) CPlayer(this, ClientID, StartTeam);
	}

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		if(ClientID >= MAX_CLIENTS-g_Config.m_DbgDummies)
			return;
	}
#endif

	// send motd
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = g_Config.m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);

	m_BroadcastStates[ClientID].m_NoChangeTick = 0;
	m_BroadcastStates[ClientID].m_LifeSpanTick = 0;
	m_BroadcastStates[ClientID].m_Priority = BROADCAST_PRIORITY_LOWEST;
	m_BroadcastStates[ClientID].m_PrevMessage[0] = 0;
	m_BroadcastStates[ClientID].m_NextMessage[0] = 0;
}

void CGameContext::OnClientDrop(int ClientID, const char *pReason)
{
	m_apPlayers[ClientID]->OnDisconnect(pReason);
	delete m_apPlayers[ClientID];
	m_apPlayers[ClientID] = 0;

	// update spectator modes
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->m_SpectatorID == ClientID)
			m_apPlayers[i]->m_SpectatorID = SPEC_FREEVIEW;
	}
}

void CGameContext::OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	void *pRawMsg = m_NetObjHandler.SecureUnpackMsg(MsgID, pUnpacker);
	CPlayer *pPlayer = m_apPlayers[ClientID];
	
	if(!pRawMsg)
	{
		if(g_Config.m_Debug) 
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "dropped weird message '%s' (%d), failed on '%s'", m_NetObjHandler.GetMsgName(MsgID), MsgID, m_NetObjHandler.FailedMsgOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
		return;
	}

	if(Server()->ClientIngame(ClientID))
	{
		if(MsgID == NETMSGTYPE_CL_SAY)
		{
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat+Server()->TickSpeed() > Server()->Tick())
				return;


			CNetMsg_Cl_Say *pMsg = (CNetMsg_Cl_Say *)pRawMsg;
			int Team = CGameContext::CHAT_ALL;
			if(pMsg->m_Team)
			{
				if(pPlayer->GetTeam() == TEAM_SPECTATORS) Team = CGameContext::CHAT_SPEC;
				else Team = CGameContext::CHAT_BLUE;
			}
			
			// trim right and set maximum length to 271 utf8-characters
			int Length = 0;
			const char *p = pMsg->m_pMessage;
			const char *pEnd = 0;
			while(*p)
 			{
				const char *pStrOld = p;
				int Code = str_utf8_decode(&p);

				// check if unicode is not empty
				if(Code > 0x20 && Code != 0xA0 && Code != 0x034F && (Code < 0x2000 || Code > 0x200F) && (Code < 0x2028 || Code > 0x202F) &&
					(Code < 0x205F || Code > 0x2064) && (Code < 0x206A || Code > 0x206F) && (Code < 0xFE00 || Code > 0xFE0F) &&
					Code != 0xFEFF && (Code < 0xFFF9 || Code > 0xFFFC))
				{
					pEnd = 0;
				}
				else if(pEnd == 0)
					pEnd = pStrOld;

				if(++Length >= 270)
				{
					*(const_cast<char *>(p)) = 0;
					break;
				}
 			}
			if(pEnd != 0)
				*(const_cast<char *>(pEnd)) = 0;

			// drop empty and autocreated spam messages (more than 16 characters per second)
			if(Length == 0 || (g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat+Server()->TickSpeed()*((15+Length)/16) > Server()->Tick()))
				return;

			pPlayer->m_LastChat = Server()->Tick();
		
		
			if(str_comp_num(pMsg->m_pMessage, "/msg ", 5) == 0)
			{
				PrivateMessage(pMsg->m_pMessage+5, ClientID, (Team != CGameContext::CHAT_ALL));
			}
			else if(str_comp_num(pMsg->m_pMessage, "/w ", 3) == 0)
			{
				PrivateMessage(pMsg->m_pMessage+3, ClientID, (Team != CGameContext::CHAT_ALL));
			}
			else if(pMsg->m_pMessage[0]=='/')
				pPlayer->m_pChatCmd->ChatCmd(pMsg);
			else
				SendChat(ClientID, Team, pMsg->m_pMessage);		
		}
		else if(MsgID == NETMSGTYPE_CL_CALLVOTE)
		{
			CNetMsg_Cl_CallVote *pMsg = (CNetMsg_Cl_CallVote *)pRawMsg;
			const char *pReason = pMsg->m_Reason[0] ? pMsg->m_Reason : "No reason given";
			
			if(str_comp_nocase(pMsg->m_Type, "kick") == 0)
			{
				int KickID = str_toint(pMsg->m_Value);
				if(KickID < 0 || KickID >= MAX_NOBOT || !m_apPlayers[KickID])
				{
					SendChatTarget(ClientID, "Invalid client id to kick");
					return;
				}
				if(KickID == ClientID)
				{
					SendChatTarget(ClientID, "你不能把你自己踢出房间");
					return;
				}
				if(Server()->IsAuthed(KickID))
				{
					SendChatTarget(ClientID, "你不能把管理员踢出房间");
					char aBufKick[128];
					str_format(aBufKick, sizeof(aBufKick), "'%s' 投票把你踢出房间", Server()->ClientName(ClientID));
					SendChatTarget(KickID, aBufKick);
					return;
				}
			}
			else
			{
				/*if(m_VoteCloseTime)
				{
					SendChatTarget(ClientID, "Wait for current vote to end before calling a new one.");
					return;
				}*/

				char aDesc[VOTE_DESC_LENGTH] = {0};
				char aCmd[VOTE_CMD_LENGTH] = {0};

				

				if(str_comp_nocase(pMsg->m_Type, "option") == 0)
				{
					for (int i = 0; i < m_PlayerVotes[ClientID].size(); ++i)
					{
						if(str_comp_nocase(pMsg->m_Value, m_PlayerVotes[ClientID][i].m_aDescription) == 0)
						{
							str_format(aDesc, sizeof(aDesc), "%s", m_PlayerVotes[ClientID][i].m_aDescription);
							str_format(aCmd, sizeof(aCmd), "%s", m_PlayerVotes[ClientID][i].m_aCommand);
				
							m_PlayerVotes[ClientID][i].data;
						}
					}
				}

				if(str_comp(aCmd, "null") == 0)
					return;
				
				// ИНФОРМАЦИЯ ФУНКЦИИ
				
				else if(str_comp(aCmd, "info") == 0)
				{
					const char* pLanguage = m_apPlayers[ClientID]->GetLanguage();
	
					dynamic_string Buffer;
					Server()->Localization()->Format_L(Buffer, pLanguage, _("All owners InfClass"), NULL); 
					Buffer.append("\n\n");
					Server()->Localization()->Format_L(Buffer, pLanguage, _("Main contributors:\nNajvlad - Mapper\nRem1x - Helping\nMatodor - Helping\nKurosio - Code, Owner"), NULL); 
					Buffer.append("\n\n");
					Server()->Localization()->Format_L(Buffer, pLanguage, _("Created Modify rAzataz by Kurosio"), NULL); 
					Buffer.append("\n\n");
	
					SendMOTD(ClientID, Buffer.buffer());
					return;
				}			

				else if(str_comp(aCmd, "rrul") == 0)
				{
					SendChatTarget(ClientID, "------- {Rules} -------");
					SendChatTarget(ClientID, "禁止使用游戏漏洞!");
					SendChatTarget(ClientID, "禁止使用辅助性软件!");
					SendChatTarget(ClientID, "禁止使用dummy!");
					SendChatTarget(ClientID, "禁止分享一个账号!");
					SendChatTarget(ClientID, "禁止侮辱玩家!");
					SendChatTarget(ClientID, "禁止进行线下交易!");
					SendChatTarget(ClientID, "最好不要干涉别人的游戏过程!");
					return;
				}					

				else if(str_comp(aCmd, "help") == 0)
				{
					const char* pLanguage = m_apPlayers[ClientID]->GetLanguage();
	
					dynamic_string Buffer;
	
					Server()->Localization()->Format_L(Buffer, pLanguage, _("你需要登录(/login)或者注册(/register)一个新的账户."), NULL); 
					Buffer.append("\n\n");
					Server()->Localization()->Format_L(Buffer, pLanguage, _("/register <用户名> <密码> - 创建一个新的账户"), NULL); 
					Buffer.append("\n");
					Server()->Localization()->Format_L(Buffer, pLanguage, _("/login <用户名> <密码> - 登录账户"), NULL); 
					Buffer.append("\n");
					Server()->Localization()->Format_L(Buffer, pLanguage, _("/logout - 注销该账户"), NULL); 
					Buffer.append("\n\n");

					SendMOTD(ClientID, Buffer.buffer());
					return;
				}	
				
				// МЕНЮ ФУНКЦИИ
						
				else if(str_comp(aCmd, "back") == 0)
				{
					ResetVotes(ClientID, m_apPlayers[ClientID]->m_LastVotelist);
					return;
				}		
												

				// КЛАН ФУНКЦИИ
				else if(str_comp(aCmd, "houseopen") == 0)
				{
					if(!Server()->GetLeader(ClientID, Server()->GetClanID(ClientID)))
					{
						SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("# 你不是该公会的会长"), NULL);
						return;
					}
					if(Server()->SetOpenHouse(Server()->GetOwnHouse(ClientID)))
					{
						SendChatTarget_Localization(-1, -1, _("公会 {str:name} {str:type} 的房屋!"), 
							"name", Server()->GetClanName(Server()->GetClanID(ClientID)), "type", Server()->GetOpenHouse(Server()->GetOwnHouse(ClientID)) ? "OPEN" : "CLOSE");
					}
					ResetVotes(ClientID, CHOUSE);
					return;
				}				

				else if(str_comp(aCmd, "ckickoff") == 0)
				{
					if(Server()->GetLeader(ClientID, Server()->GetClanID(ClientID)))
					{	
						if(str_comp_nocase(m_apPlayers[ClientID]->m_SelectPlayer, Server()->ClientName(ClientID)) == 0)
							return;
						
						bool Type = false;
						for(int i = 0; i < MAX_NOBOT; ++i)
						{
							if(m_apPlayers[i])
							{
								if(str_comp_nocase(m_apPlayers[ClientID]->m_SelectPlayer, Server()->ClientName(i)) == 0)
									Type = true;
							}
						}
					
						if(Type) SendChatClan(Server()->GetClanID(ClientID), "会长将玩家 {str:name} 踢出了公会", "name", m_apPlayers[ClientID]->m_SelectPlayer);
						else SendChatClan(Server()->GetClanID(ClientID), "会长将离线玩家 {str:name} 提出了公会", "name", m_apPlayers[ClientID]->m_SelectPlayer);

						Server()->ExitClanOff(ClientID, m_apPlayers[ClientID]->m_SelectPlayer);			
						ResetVotes(ClientID, CLANLIST);
					}
					else
						SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("# 你不是该公会的会长"), NULL);
												
					return;
				}	
				
				else if(str_comp(aCmd, "cgetleader") == 0)
				{
					if(Server()->GetLeader(ClientID, Server()->GetClanID(ClientID)))
					{	
						if(str_comp_nocase(m_apPlayers[ClientID]->m_SelectPlayer, Server()->ClientName(ClientID)) == 0)
							return;
						
						bool heh = false;
						for(int i = 0; i < MAX_NOBOT; ++i){
							if(m_apPlayers[i]){
								if(Server()->GetClanID(i) == Server()->GetClanID(ClientID)){
										
									heh = true;
									SendChatTarget_Localization(i, CHATCATEGORY_DEFAULT, _("公会新的会长:{str:name}"), "name", m_apPlayers[ClientID]->m_SelectPlayer, NULL);	
								}
							}
						}
						
						if(heh)
						{
							Server()->ChangeLeader(Server()->GetClanID(ClientID), m_apPlayers[ClientID]->m_SelectPlayer); 
							ResetVotes(ClientID, CLANLIST);
							
						}
					}
					else
						SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("# 你不是该公会的会长"), NULL);
												
					return;
				}	
				
				else if(str_comp(aCmd, "cexit") == 0)
				{
					if(Server()->GetLeader(ClientID, Server()->GetClanID(ClientID)))
					{
						SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("会长不能退出公会"), NULL);							
						return;
					}
					ExitClan(ClientID);
					return;
				}	
				
				else if(str_comp(aCmd, "uccount") == 0)
				{						
					if(Server()->GetClan(DMAXCOUNTUCLAN, Server()->GetClanID(ClientID)) >= 20)
						return 	SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("Max upgrades"), NULL);

					BuyUpgradeClan(ClientID, (m_apPlayers[ClientID]->GetNeedForUpgClan(DMAXCOUNTUCLAN))*4, DMAXCOUNTUCLAN,"MaxNum");	
					return;
				}	
				else if(str_comp(aCmd, "uaddexp") == 0)
					BuyUpgradeClan(ClientID, m_apPlayers[ClientID]->GetNeedForUpgClan(DADDEXP), DADDEXP,"ExpAdd");							

				else if(str_comp(aCmd, "uchair") == 0)
				{
					if(!Server()->GetHouse(ClientID))
						return 	SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你所在的公会没房屋!"), NULL);

					BuyUpgradeClan(ClientID, m_apPlayers[ClientID]->GetNeedForUpgClan(DCHAIRHOUSE)*2, DCHAIRHOUSE,"ChairHouse");							
				}

				else if(str_comp(aCmd, "uspawnhouse") == 0)
				{
					if(!Server()->GetHouse(ClientID))
						return 	SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你所在的公会没房屋!"), NULL);

					if(Server()->GetSpawnInClanHouse(ClientID, 0) || Server()->GetSpawnInClanHouse(ClientID, 1))
						return;

					BuyUpgradeClan(ClientID, 500000, DADDEXP,"SpawnHouse");							
				}	

				else if(str_comp(aCmd, "uaddmoney") == 0)
					BuyUpgradeClan(ClientID, m_apPlayers[ClientID]->GetNeedForUpgClan(DADDMONEY), DADDEXP,"MoneyAdd");	
								
				else if(str_comp(aCmd, "cm1") == 0)
				{
					long int Get = 1000; 
					if (pReason[0] && isdigit(pReason[0]))
						Get = atoi (pReason);
						
					if(m_apPlayers[ClientID]->AccData.Gold < Get)				
						Get = m_apPlayers[ClientID]->AccData.Gold;
					
					if(Get < 1 || Get > 100000000)
						Get = 1000;
					
					if(m_apPlayers[ClientID]->AccData.Gold > 0)
					{						
						Server()->InitClanID(Server()->GetClanID(ClientID), PLUS, "Money", Get, true);

						m_apPlayers[ClientID]->AccData.Gold -= Get;
						m_apPlayers[ClientID]->AccData.ClanAdded += Get;
						SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你向工会捐了 {int:count} 黄金"), "count", &Get, NULL);							
						
						UpdateStats(ClientID);
						ResetVotes(ClientID, AUTH);
						return;
					}
					else SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有那么多钱,小穷光蛋"), NULL);							
					
					return;
				}	

				// КВЕСТЫ ФУНКЦИИ -- 寻宝游戏功能? -- 任务相关
				
				else if(str_comp(aCmd, "passquest") == 0)
				{
					if(m_apPlayers[ClientID]->AccData.Quest == 1)
					{
						if(Server()->GetItemCount(ClientID, PIGPORNO) < QUEST1)
							return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("任务还未完成!"), NULL);	
						else
						{
							m_apPlayers[ClientID]->ExpAdd(4000);
							m_apPlayers[ClientID]->MoneyAdd(200000);
							m_apPlayers[ClientID]->AccData.Quest++;
							Server()->RemItem(ClientID, 2, QUEST1, -1);
							UpdateStats(ClientID);
						}
					}

					if(m_apPlayers[ClientID]->AccData.Quest == 2)
					{
						if(Server()->GetItemCount(ClientID, PIGPORNO) < QUEST2)
							return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("任务还未完成!"), NULL);	
						else
						{
							m_apPlayers[ClientID]->ExpAdd(4000);
							m_apPlayers[ClientID]->MoneyAdd(250000);
							m_apPlayers[ClientID]->AccData.Quest++;
							Server()->RemItem(ClientID, 2, QUEST2, -1);
							UpdateStats(ClientID);
						}
					}

					else if(m_apPlayers[ClientID]->AccData.Quest == 3)
					{
						if(Server()->GetItemCount(ClientID, KWAHGANDON) < QUEST3)
							return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("任务还未完成!"), NULL);	
						else
						{
							m_apPlayers[ClientID]->ExpAdd(8000);
							m_apPlayers[ClientID]->MoneyAdd(500000);
							m_apPlayers[ClientID]->AccData.Quest++;
							Server()->RemItem(ClientID, 3, QUEST3, -1);
							UpdateStats(ClientID);
						}
					}

					else if(m_apPlayers[ClientID]->AccData.Quest == 4)
					{
						if(Server()->GetItemCount(ClientID, KWAHGANDON) < QUEST4)
							return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("任务还未完成!"), NULL);	
						else
						{
							m_apPlayers[ClientID]->ExpAdd(8000);
							m_apPlayers[ClientID]->MoneyAdd(550000);
							m_apPlayers[ClientID]->AccData.Quest++;
							Server()->RemItem(ClientID, 3, QUEST4, -1);
							UpdateStats(ClientID);
						}
					}

					else if(m_apPlayers[ClientID]->AccData.Quest == 5)
					{
						if(Server()->GetItemCount(ClientID, KWAHGANDON) < QUEST5 || Server()->GetItemCount(ClientID, PIGPORNO) < QUEST3)
							return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("任务还未完成!"), NULL);	
						else
						{
							m_apPlayers[ClientID]->MoneyAdd(1000000);
							m_apPlayers[ClientID]->AccData.Quest++;
							Server()->RemItem(ClientID, KWAHGANDON, QUEST5, -1);
							Server()->RemItem(ClientID, PIGPORNO, QUEST5, -1);
							SendMail(ClientID, "Hello, 任务与报酬系统!", EARRINGSKWAH, 1);
							UpdateStats(ClientID);
						}
					}
					else if(m_apPlayers[ClientID]->AccData.Quest == 6)
					{
						if(Server()->GetItemCount(ClientID, KWAHGANDON) < QUEST6 || Server()->GetItemCount(ClientID, FOOTKWAH) < QUEST6)
							return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("任务还未完成!"), NULL);	
						else
						{
							m_apPlayers[ClientID]->MoneyAdd(1050000);
							m_apPlayers[ClientID]->AccData.Quest++;
							Server()->RemItem(ClientID, KWAHGANDON, QUEST6, -1);
							Server()->RemItem(ClientID, FOOTKWAH, QUEST6, -1);
							SendMail(ClientID, "Hello, 任务与报酬系统!", FORMULAWEAPON, 1);
							SendMail(ClientID, "Hello, 你解锁了一个新的称号!", TITLEQUESTS, 1);
							UpdateStats(ClientID);
						}
					}
					ResetVotes(ClientID, QUEST);	
					
					return;
				}	
				
				// ПРОКАЧКА ФУНКЦИИ
				
				else if(str_comp(aCmd, "uhealth") == 0)
				{
					int Get = 1;
					if (pReason[0] && isdigit(pReason[0]))
						Get = atoi (pReason);
						
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < Get)				
						Get = m_apPlayers[ClientID]->AccUpgrade.Upgrade;
					
					if(Get < 1 || Get > 1000)
						Get = 1;
					
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade <= 0)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Health >= BMAXHEALTH) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Health >= HMAXHEALTH) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Health >= AMAXHEALTH))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
					
					m_apPlayers[ClientID]->AccUpgrade.Health += Get;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= Get;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地升了 {int:lv} 级"), "lv", &Get, NULL);							
					
					if(m_apPlayers[ClientID]->GetCharacter())
					{
						if(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER)
							m_apPlayers[ClientID]->m_HealthStart += Get*50;
						else if(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK)
							m_apPlayers[ClientID]->m_HealthStart += Get*40;
						else if(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS)
							m_apPlayers[ClientID]->m_HealthStart += Get*40;
					}
						
					UpdateUpgrades(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				
				else if(str_comp(aCmd, "udamage") == 0)
				{
					int Get = 1;
					if (pReason[0] && isdigit(pReason[0]))
						Get = atoi (pReason);
						
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < Get)				
						Get = m_apPlayers[ClientID]->AccUpgrade.Upgrade;

					int GetSize = 0;
					switch(m_apPlayers[ClientID]->GetClass())
					{
						case PLAYERCLASS_ASSASINS: GetSize = AMAXDAMAGE-m_apPlayers[ClientID]->AccUpgrade.Damage; break;
						case PLAYERCLASS_BERSERK: GetSize = BMAXDAMAGE-m_apPlayers[ClientID]->AccUpgrade.Damage; break;
						case PLAYERCLASS_HEALER: GetSize = HMAXDAMAGE-m_apPlayers[ClientID]->AccUpgrade.Damage; break;
					}
					if(Get > GetSize)
						Get = GetSize;
					
					if(Get < 1 || Get > 1000)
						Get = 1;
					
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade <= 0)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Damage >= BMAXDAMAGE) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Damage >= HMAXDAMAGE) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Damage >= AMAXDAMAGE))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
				
					m_apPlayers[ClientID]->AccUpgrade.Damage += Get;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= Get;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地升了 {int:lv} 级"), "lv", &Get, NULL);							
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Damage > BMAXDAMAGE-1)||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Damage > HMAXDAMAGE-1)||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Damage > AMAXDAMAGE-1))
					{
						SendMail(ClientID, "这已经是最后一件了!", SNAPDAMAGE, 1);	
					}
					
					UpdateUpgrades(ClientID);
					ResetVotes(ClientID, CLMENU);	
					
					return;
				}	
				
				else if(str_comp(aCmd, "uammo") == 0)
				{
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < 5)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Ammo >= BMAXAMMO) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Ammo >= HMAXAMMO) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Ammo >= AMAXAMMO))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
									
					m_apPlayers[ClientID]->AccUpgrade.Ammo++;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= 5;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地获得了升级"), NULL);							
					
					int geta = (int)(5+m_apPlayers[ClientID]->AccUpgrade.Ammo);
					Server()->SetMaxAmmo(ClientID, INFWEAPON_GUN, geta);
					Server()->SetMaxAmmo(ClientID, INFWEAPON_SHOTGUN, geta);
					Server()->SetMaxAmmo(ClientID, INFWEAPON_GRENADE, geta);
					Server()->SetMaxAmmo(ClientID, INFWEAPON_RIFLE, geta);
					
					UpdateUpgrades(ClientID);	
					ResetVotes(ClientID, CLMENU);	
					
					return;
				}	
				else if(str_comp(aCmd, "uammoregen") == 0)
				{
					int Get = 1;
					if (pReason[0] && isdigit(pReason[0]))
						Get = atoi (pReason);
						
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < Get)				
						Get = m_apPlayers[ClientID]->AccUpgrade.Upgrade;

					int GetSize = 0;
					switch(m_apPlayers[ClientID]->GetClass())
					{
						case PLAYERCLASS_ASSASINS: GetSize = AMAXAREGEN-m_apPlayers[ClientID]->AccUpgrade.AmmoRegen; break;
						case PLAYERCLASS_BERSERK: GetSize = BMAXAREGEN-m_apPlayers[ClientID]->AccUpgrade.AmmoRegen; break;
						case PLAYERCLASS_HEALER: GetSize = HMAXAREGEN-m_apPlayers[ClientID]->AccUpgrade.AmmoRegen; break;
					}
					if(Get > GetSize)
						Get = GetSize;

					if(Get < 1 || Get > 1000)
						Get = 1;

					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade <= 0)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.AmmoRegen >= BMAXAREGEN) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.AmmoRegen >= HMAXAREGEN) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.AmmoRegen >= AMAXAREGEN))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
									
					m_apPlayers[ClientID]->AccUpgrade.AmmoRegen += Get;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= Get;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地升了 {int:lv} 级"), "lv", &Get, NULL);							
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.AmmoRegen > BMAXAREGEN-1)||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.AmmoRegen > HMAXAREGEN-1)||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.AmmoRegen > AMAXAREGEN-1))
					{
						SendMail(ClientID, "You got finnaly item!", SNAPAMMOREGEN, 1);	
					}
					
					int getar = (int)(650-m_apPlayers[ClientID]->AccUpgrade.AmmoRegen*2);
					Server()->SetAmmoRegenTime(ClientID, INFWEAPON_GRENADE, getar);
					Server()->SetAmmoRegenTime(ClientID, INFWEAPON_SHOTGUN, getar);
					Server()->SetAmmoRegenTime(ClientID, INFWEAPON_RIFLE, getar);
					
					UpdateUpgrades(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				else if(str_comp(aCmd, "uhandle") == 0)
				{
					int Get = 1;
					if (pReason[0] && isdigit(pReason[0]))
						Get = atoi (pReason);
										
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < Get)				
						Get = m_apPlayers[ClientID]->AccUpgrade.Upgrade;

					int GetSize = 0;
					switch(m_apPlayers[ClientID]->GetClass())
					{
						case PLAYERCLASS_ASSASINS: GetSize = AMAXHANDLE-m_apPlayers[ClientID]->AccUpgrade.Speed; break;
						case PLAYERCLASS_BERSERK: GetSize = BMAXHANDLE-m_apPlayers[ClientID]->AccUpgrade.Speed; break;
						case PLAYERCLASS_HEALER: GetSize = HMAXHANDLE-m_apPlayers[ClientID]->AccUpgrade.Speed; break;
					}
					if(Get > GetSize)
						Get = GetSize;

					if(Get < 1 || Get > 1000)
						Get = 1;

					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade <= 0)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Speed >= BMAXHANDLE) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Speed >= HMAXHANDLE) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Speed >= AMAXHANDLE))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
					
					m_apPlayers[ClientID]->AccUpgrade.Speed += Get;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= Get;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地升了 {int:lv} 级"), "lv", &Get, NULL);							
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Speed > BMAXHANDLE-1)||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Speed > HMAXHANDLE-1)||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Speed > AMAXHANDLE-1))
					{
						SendMail(ClientID, "You got finnaly item!", SNAPHANDLE, 1);
					}
					
					int getsp = (int)(1000+m_apPlayers[ClientID]->AccUpgrade.Speed*30);
					int getspg = (int)(1000+m_apPlayers[ClientID]->AccUpgrade.Speed*12);
					Server()->SetFireDelay(ClientID, INFWEAPON_HAMMER, getsp);	
					Server()->SetFireDelay(ClientID, INFWEAPON_GUN, getsp);
					Server()->SetFireDelay(ClientID, INFWEAPON_SHOTGUN, getsp);
					Server()->SetFireDelay(ClientID, INFWEAPON_GRENADE, getspg);
					Server()->SetFireDelay(ClientID, INFWEAPON_RIFLE, getsp);
					
					UpdateUpgrades(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	

				else if(str_comp(aCmd, "umana") == 0)
				{
					int Get = 1;
					if (pReason[0] && isdigit(pReason[0]))
						Get = atoi (pReason);
										
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < Get)				
						Get = m_apPlayers[ClientID]->AccUpgrade.Upgrade;

					if(Get < 1 || Get > 1000)
						Get = 1;

					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade <= 0)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					m_apPlayers[ClientID]->AccUpgrade.Mana += Get;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= Get;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地升了 {int:lv} 级"), "lv", &Get, NULL);							
	
					UpdateUpgrades(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	

				else if(str_comp(aCmd, "uspray") == 0)
				{
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < 10)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Spray >= BMAXSPREED) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Spray >= HMAXSPREED) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Spray >= AMAXSPREED))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
					
					m_apPlayers[ClientID]->AccUpgrade.Spray++;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= 10;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地获得了升级"), NULL);							
					
					UpdateUpgrades(ClientID);
					ResetVotes(ClientID, CLMENU);	
					
					return;
				}	
				
				else if(str_comp(aCmd, "uhpregen") == 0)
				{
					int Get = 1;
					if (pReason[0] && isdigit(pReason[0]))
						Get = atoi (pReason);

					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade < Get)				
						Get = m_apPlayers[ClientID]->AccUpgrade.Upgrade;

					int GetSize = 0;
					switch(m_apPlayers[ClientID]->GetClass())
					{
						case PLAYERCLASS_ASSASINS: GetSize = AMAXHPREGEN-m_apPlayers[ClientID]->AccUpgrade.HPRegen; break;
						case PLAYERCLASS_BERSERK: GetSize = BMAXHPREGEN-m_apPlayers[ClientID]->AccUpgrade.HPRegen; break;
						case PLAYERCLASS_HEALER: GetSize = HMAXHPREGEN-m_apPlayers[ClientID]->AccUpgrade.HPRegen; break;
					}
					if(Get > GetSize)
						Get = GetSize;
					
					if(Get < 1 || Get > 1000)
						Get = 1;
					
					if(m_apPlayers[ClientID]->AccUpgrade.Upgrade <= 0)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的升级点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.HPRegen >= BMAXHPREGEN) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.HPRegen >= HMAXHPREGEN) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.HPRegen >= AMAXHPREGEN))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
							
					m_apPlayers[ClientID]->AccUpgrade.HPRegen += Get;
					m_apPlayers[ClientID]->AccUpgrade.Upgrade -= Get;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地升了 {int:lv} 级"), "lv", &Get, NULL);							
					
					UpdateUpgrades(ClientID);	
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				
				else if(str_comp(aCmd, "ushammerrange") == 0)
				{
					if(m_apPlayers[ClientID]->AccUpgrade.SkillPoint < HAMMERRANGE)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的技能点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.HammerRange > 4) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.HammerRange > 5) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.HammerRange > 7))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	

					m_apPlayers[ClientID]->AccUpgrade.HammerRange++;
					m_apPlayers[ClientID]->AccUpgrade.SkillPoint -= HAMMERRANGE;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你的技能成功地提升了"), NULL);							
						
					UpdateUpgrades(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				
				else if(str_comp(aCmd, "upasive2") == 0)
				{
					if(m_apPlayers[ClientID]->AccUpgrade.SkillPoint < HAMMERRANGE)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的技能点"), NULL);	
					
					if((m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK && m_apPlayers[ClientID]->AccUpgrade.Pasive2 > 4) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER && m_apPlayers[ClientID]->AccUpgrade.Pasive2 > 10) ||
						(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS && m_apPlayers[ClientID]->AccUpgrade.Pasive2 > 7))
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	
					
					m_apPlayers[ClientID]->AccUpgrade.Pasive2++;
					m_apPlayers[ClientID]->AccUpgrade.SkillPoint -= HAMMERRANGE;
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你的技能成功地提升了"), NULL);							
						
					UpdateUpgrades(ClientID);
						
					ResetVotes(ClientID, CLMENU);	
					   
					return;
				}	

				else if(str_comp(aCmd, "uskillwall") == 0)
				{
					BuySkill(ClientID, 70, SKWALL); 
					return;
				}	

				else if(str_comp(aCmd, "uskillsword") == 0)
				{
					BuySkill(ClientID, 20, SSWORD);
					return;
				}	

				else if(str_comp(aCmd, "uskillheal") == 0)
				{
					BuySkill(ClientID, 60, SKHEAL);
					return;
				}	
				
				// МАГАЗИН ФУНКЦИИ
				else if(str_comp(aCmd, "bvip") == 0)
				{
					Server()->SetItemPrice(ClientID, VIPPACKAGE, 0, 1000);
					BuyItem(VIPPACKAGE, ClientID, 1);
					return;
				}	
				else if(str_comp(aCmd, "bsp") == 0)
				{
					Server()->SetItemPrice(ClientID, SKILLUPBOX, 0, 200);
					BuyItem(SKILLUPBOX, ClientID, 1);
					return;
				}	
				else if(str_comp(aCmd, "bantipvp") == 0)
				{
					Server()->SetItemPrice(ClientID, SANTIPVP, 0, 200);
					BuyItem(SANTIPVP, ClientID, 1);
					return;
				}
				
				// НАСТРОЙКИ ФУНКЦИИ
				else if(str_comp(aCmd, "ssantiping") == 0)
				{
					if(Server()->GetClientAntiPing(ClientID))
						Server()->SetClientAntiPing(ClientID, 0);
					else 
						Server()->SetClientAntiPing(ClientID, 1);
						
					ResetVotes(ClientID, SETTINGS);	
					return;
				}	
				else if(str_comp(aCmd, "ssseccurity") == 0)
				{
					if(Server()->GetSeccurity(ClientID))
						Server()->SetSeccurity(ClientID, 0);
					else 
						Server()->SetSeccurity(ClientID, 1);
						
					ResetVotes(ClientID, SETTINGS);	
					return;
				}	
					
				else if(str_comp(aCmd, "sssetingschat") == 0)
				{
					int Get = Server()->GetItemSettings(ClientID, SCHAT)+1;
					if(Get > 2) Server()->SetItemSettingsCount(ClientID, SCHAT, 0);
					else Server()->SetItemSettingsCount(ClientID, SCHAT, Get);
						
					UpdateStats(ClientID);
					ResetVotes(ClientID, SETTINGS);	
					return;
				}	
				else if(str_comp(aCmd, "sskillwall") == 0)
				{
					int Get = Server()->GetItemSettings(ClientID, SKWALL)+1;
					if(Get > 7) Server()->SetItemSettingsCount(ClientID, SKWALL, 0);
					else Server()->SetItemSettingsCount(ClientID, SKWALL, Get);
						
					UpdateStats(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				else if(str_comp(aCmd, "sskillheal") == 0)
				{
					int Get = Server()->GetItemSettings(ClientID, SKHEAL)+1;
					if(Get > 7) Server()->SetItemSettingsCount(ClientID, SKHEAL, 0);
					else Server()->SetItemSettingsCount(ClientID, SKHEAL, Get);
						
					UpdateStats(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				else if(str_comp(aCmd, "sskillsword") == 0)
				{
					int Get = Server()->GetItemSettings(ClientID, SSWORD)+1;
					if(Get > 7) Server()->SetItemSettingsCount(ClientID, SSWORD, 0);
					else Server()->SetItemSettingsCount(ClientID, SSWORD, Get);
						
					UpdateStats(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				else if(str_comp(aCmd, "sskillsummer") == 0)
				{
					int Get = Server()->GetItemSettings(ClientID, SHEALSUMMER)+1;
					if(Get > 7) Server()->SetItemSettingsCount(ClientID, SHEALSUMMER, 0);
					else Server()->SetItemSettingsCount(ClientID, SHEALSUMMER, Get);
						
					UpdateStats(ClientID);
					ResetVotes(ClientID, CLMENU);	
					return;
				}	
				else if(str_comp(aCmd, "ssantipvp") == 0)
				{
					Server()->SetItemSettings(ClientID, SANTIPVP);
					ResetVotes(ClientID, SETTINGS);	
					return;
				}		
				else if(str_comp(aCmd, "sssetingsdrop") == 0)
				{
					Server()->SetItemSettings(ClientID, SDROP);
						
					UpdateStats(ClientID);
					ResetVotes(ClientID, SETTINGS);	
					return;
				}		

				// ИНВЕНТАРЬ ФУНКЦИИ 
				else if(str_comp(aCmd, "useitem") == 0)
				{	
					if(m_apPlayers[ClientID]->m_LastChangeInfo && m_apPlayers[ClientID]->m_LastChangeInfo+Server()->TickSpeed()*3 > Server()->Tick())
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("请等待..."), NULL);

					m_apPlayers[ClientID]->m_LastChangeInfo = Server()->Tick();
					int SelectItem = m_apPlayers[ClientID]->m_SelectItem;
					
					int Get = chartoint(pReason, MAX_COUNT );
					if(SelectItem == RANDOMCRAFTITEM || SelectItem == EVENTBOX || SelectItem == FARMBOX ||
						SelectItem == RESETINGUPGRADE || SelectItem == RESETINGSKILL || SelectItem == VIPPACKAGE)
						Get = 1;
					
					Server()->RemItem(ClientID, SelectItem, Get, USEDUSE);
					m_apPlayers[ClientID]->m_SelectItem = -1;
					ResetVotes(ClientID, AUTH);
					return;
				}	
				
				else if(str_comp(aCmd, "sellitem") == 0)
				{	
					int SelectItem = m_apPlayers[ClientID]->m_SelectItem;

					Server()->RemItem(ClientID, SelectItem, chartoint(pReason, MAX_COUNT), USEDSELL);
					m_apPlayers[ClientID]->m_SelectItem = -1;
					return;
				}	
				
				else if(str_comp(aCmd, "dropitem") == 0)
				{
					if(m_apPlayers[ClientID]->m_LastChangeInfo && m_apPlayers[ClientID]->m_LastChangeInfo+Server()->TickSpeed()*3 > Server()->Tick())
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("请稍候..."), NULL);

					m_apPlayers[ClientID]->m_LastChangeInfo = Server()->Tick();
					int SelectItem = m_apPlayers[ClientID]->m_SelectItem;					
					Server()->RemItem(ClientID, SelectItem, chartoint(pReason, MAX_COUNT), USEDDROP); // Выброс предметов для всех игроков
					m_apPlayers[ClientID]->m_SelectItem = -1;
					ResetVotes(ClientID, AUTH);
					return;
				}	
				
				else if(str_comp(aCmd, "enchantitem") == 0)
				{
					if(m_apPlayers[ClientID]->m_LastChangeInfo && m_apPlayers[ClientID]->m_LastChangeInfo+Server()->TickSpeed()*3 > Server()->Tick())
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("请等待..."), NULL);

					m_apPlayers[ClientID]->m_LastChangeInfo = Server()->Tick();
					int SelectItem = m_apPlayers[ClientID]->m_SelectItem;					
					if(Server()->GetItemCount(ClientID, MATERIAL) < 1000)
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("需要 1000 材料(material)"), NULL); 	

					Server()->RemItem(ClientID, MATERIAL, 1000, -1);
					if(rand()%(1+(Server()->GetItemEnchant(ClientID, SelectItem))) == 0)
					{
						Server()->SetItemEnchant(ClientID, SelectItem, Server()->GetItemEnchant(ClientID, SelectItem)+1);

						int Enchant = Server()->GetItemEnchant(ClientID, SelectItem);
						SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 成功地附魔了物品:{str:item} +{int:enchant}"), 
							"name", Server()->ClientName(ClientID), "item", Server()->GetItemName(ClientID, SelectItem), "enchant", &Enchant, NULL); 
					
						if(Enchant == 10 && !Server()->GetItemCount(ClientID, TITLEENCHANT))
						{
							SendMail(ClientID, "Hello, 你解锁了一个新的称号!", TITLEENCHANT, 1);
						}
					}
					else SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("升级失败."), NULL); 
				
					m_apPlayers[ClientID]->m_SelectItem = -1;
					ResetVotes(ClientID, ARMORMENU);
					return;
				}	

				else if(str_comp(aCmd, "destitem") == 0)
				{
					if(m_apPlayers[ClientID]->m_LastChangeInfo && m_apPlayers[ClientID]->m_LastChangeInfo+Server()->TickSpeed()*3 > Server()->Tick())
						return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("请等候..."), NULL);

					m_apPlayers[ClientID]->m_LastChangeInfo = Server()->Tick();
					int SelectItem = m_apPlayers[ClientID]->m_SelectItem;
					int Get = chartoint(pReason, MAX_COUNT);
					
					if(Server()->GetItemCount(ClientID, SelectItem) < Get)	
						Get = Server()->GetItemCount(ClientID, SelectItem);
					
					if(!Server()->GetItemCount(ClientID, SelectItem))
					{
						if(m_apPlayers[ClientID]->GetCharacter())
							switch(SelectItem)
							{
								case IGUN:	m_apPlayers[ClientID]->GetCharacter()->RemoveGun(WEAPON_GUN); break;
								case ISHOTGUN: m_apPlayers[ClientID]->GetCharacter()->RemoveGun(WEAPON_SHOTGUN); break;
								case IGRENADE: m_apPlayers[ClientID]->GetCharacter()->RemoveGun(WEAPON_GRENADE); break;
								case ILASER: m_apPlayers[ClientID]->GetCharacter()->RemoveGun(WEAPON_RIFLE); break;
							}
					}
					Server()->RemItem(ClientID, SelectItem, Get, -1);

					if(Server()->GetItemType(ClientID, SelectItem) == 15 || Server()->GetItemType(ClientID, SelectItem) == 16)
						m_pController->OnPlayerInfoChange(pPlayer);

					m_apPlayers[ClientID]->m_SelectItem = -1;
					ResetVotes(ClientID, AUTH);
					return;
				}	

				for(int i = 0; i < 20; i++)
				{
					char aBuf[16];
					str_format(aBuf, sizeof(aBuf), "reward%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						Server()->RemMail(Server()->GetMailRewardDell(ClientID, i));
						int ItemID = Server()->GetRewardMail(ClientID, i, 0);
						int ItemNum = Server()->GetRewardMail(ClientID, i, 1);
						Server()->SetRewardMail(ClientID, i, -1, -1);
						if(ItemID != -1 && ItemNum != -1)
							GiveItem(ClientID, ItemID, ItemNum);
					
						ResetVotes(ClientID, MAILMENU);
					}
				}

				for(int i = 1; i < MAXMENU; ++i)
				{
					char aBuf[16];
					str_format(aBuf, sizeof(aBuf), "menu%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						ResetVotes(ClientID, i);
						return;	
					}						
				}	

				for(int i = 1; i < 7; ++i)
				{
					char aBuf[16];
					str_format(aBuf, sizeof(aBuf), "its%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						m_apPlayers[ClientID]->m_SelectItemType = i;
						ResetVotes(ClientID, INVENTORY);
						return;	
					}						
				}			
				for(int i = 1; i < 7; ++i)
				{
					char aBuf[16];
					str_format(aBuf, sizeof(aBuf), "scr%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						m_apPlayers[ClientID]->m_SortedSelectCraft = i;
						ResetVotes(ClientID, CRAFTING);
						return;	
					}						
				}

				for(int i = 1; i < 4; i++)
				{
					char aBuf[16];
					str_format(aBuf, sizeof(aBuf), "armor%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						m_apPlayers[ClientID]->m_SelectArmor = i;
						ResetVotes(ClientID, ARMORMENU);
						return;	
					}						
				}

				for(int i = 1; i < 9; ++i)
				{
					char aBuf[16];
					str_format(aBuf, sizeof(aBuf), "sort%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						m_apPlayers[ClientID]->m_SortedSelectTop = i;
						ResetVotes(ClientID, TOPMENU);
						return;	
					}						
				}

				// Все функции с предметами
				for(int i = 0; i < MAX_ITEM; ++i)
				{
					char aBuf[16];

					str_format(aBuf, sizeof(aBuf), "set%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						Server()->SetItemSettings(ClientID, i, Server()->GetItemType(ClientID, i));	
						ResetVotes(ClientID, m_apPlayers[ClientID]->m_UpdateMenu);	

						m_pController->OnPlayerInfoChange(m_apPlayers[ClientID]);

						if(i == PIZDAMET)
						{	
							if(Server()->GetItemSettings(ClientID, PIZDAMET))
								Server()->SetFireDelay(ClientID, INFWEAPON_GRENADE, 7000);
							else
								Server()->SetFireDelay(ClientID, INFWEAPON_GRENADE, int(1000+m_apPlayers[ClientID]->AccUpgrade.Speed*8));
						}

						if(i == LAMPHAMMER)
						{	
							if(Server()->GetItemSettings(ClientID, LAMPHAMMER))
								Server()->SetFireDelay(ClientID, INFWEAPON_HAMMER, 1200);
							else
								Server()->SetFireDelay(ClientID, INFWEAPON_HAMMER, int(1000+m_apPlayers[ClientID]->AccUpgrade.Speed*12));
						}
						return;	
					}	

					str_format(aBuf, sizeof(aBuf), "bon%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						BuyItem(i, ClientID);
						return;	
					}	

					str_format(aBuf, sizeof(aBuf), "seli%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						Server()->RemItem(ClientID, i, chartoint(pReason, MAX_COUNT), USEDSELL);
						return;	
					}	

					str_format(aBuf, sizeof(aBuf), "it%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						m_apPlayers[ClientID]->m_SelectItem = i;
						ResetVotes(ClientID, SELITEM);
						return;	
					}	

					str_format(aBuf, sizeof(aBuf), "cra%d", i);
					if(str_comp(aCmd, aBuf) == 0)
					{
						CreateItem(ClientID, i, 1);
						return;	
					}	
				}	
				
				for(int i = 0; i < 64; ++i)
				{
					char aBuf[16];
					str_format(aBuf, sizeof(aBuf), "cs%d", i);
					
					if(str_comp(aCmd, aBuf) == 0)
					{
						str_copy(m_apPlayers[ClientID]->m_SelectPlayer, Server()->GetSelectName(ClientID, i), sizeof(m_apPlayers[ClientID]->m_SelectPlayer));
						ResetVotes(ClientID, CSETTING);
						return;
					}	
				}	
			}
		}
		else if(MsgID == NETMSGTYPE_CL_VOTE)
		{	
			CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;		
			if(m_InviteTick[ClientID] > 0)
			{
				if(!pMsg->m_Vote)
					return;

				if(pMsg->m_Vote)
				{
					if(pMsg->m_Vote > 0)
					{
						if(m_apPlayers[ClientID])
						{	
							EnterClan(ClientID, m_apPlayers[ClientID]->m_InviteClanID);
							ResetVotes(ClientID, AUTH);
							SendBroadcast_Localization(ClientID, BROADCAST_PRIORITY_INTERFACE, 150, _("欢迎来到公会"), NULL);
						}
					}
					else
					{
						if(m_apPlayers[ClientID])
							SendBroadcast_Localization(ClientID, BROADCAST_PRIORITY_INTERFACE, 10, _(" "), NULL);
					}
					m_InviteTick[ClientID] = 0;

					CNetMsg_Sv_VoteSet Msg;
					Msg.m_Timeout = 0;
					Msg.m_pDescription = "";
					Msg.m_pReason = "";
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
				}
			}
			else if(m_apPlayers[ClientID]->GetCharacter())
			{
				m_apPlayers[ClientID]->GetCharacter()->PressF3orF4(ClientID, pMsg->m_Vote);
			}
		}
		else if (MsgID == NETMSGTYPE_CL_SETTEAM && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetTeam *pMsg = (CNetMsg_Cl_SetTeam *)pRawMsg;
			if(pPlayer->GetTeam() == pMsg->m_Team || pPlayer->GetTeam() != TEAM_SPECTATORS)
				return;

			if(!Server()->IsClientLogged(ClientID))
			{
				SendBroadcast_Localization(ClientID, BROADCAST_PRIORITY_GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE, _("你需要登录(/login)或者创建(/register)一个新账户"), NULL);
				return;
			}

			GetStat(ClientID);
			GetUpgrade(ClientID);

			CPlayer *pPlayer = m_apPlayers[ClientID];
			if(g_Config.m_SvCityStart == 1 && pPlayer->AccData.Level < 250)
			{ 
				SendBroadcast_Localization(ClientID, BROADCAST_PRIORITY_GAMEANNOUNCE, BROADCAST_DURATION_GAMEANNOUNCE, _("你需要 100 级"), NULL);
				return;
			}

			if(!Server()->GetItemCount(ClientID, CUSTOMSKIN))
				pPlayer->SetClassSkin(m_apPlayers[ClientID]->GetClass());

			Server()->InitClanID(Server()->GetClanID(ClientID), PLUS, "Init", 0, false);
			Server()->ListInventory(ClientID, -1, true);	
			pPlayer->SetTeam(1, false);
			ResetVotes(ClientID, AUTH);
			pPlayer->SetMoveChar();	
		}
		else if (MsgID == NETMSGTYPE_CL_SETSPECTATORMODE && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetSpectatorMode *pMsg = (CNetMsg_Cl_SetSpectatorMode *)pRawMsg;
			if(pMsg->m_SpectatorID != SPEC_FREEVIEW)
				if (!Server()->ReverseTranslate(pMsg->m_SpectatorID, ClientID))
					return;

			if((g_Config.m_SvSpamprotection && pPlayer->m_LastSetSpectatorMode && pPlayer->m_LastSetSpectatorMode+Server()->TickSpeed()/4 > Server()->Tick()))
				return;

			pPlayer->m_LastSetSpectatorMode = Server()->Tick();
			if(pMsg->m_SpectatorID != SPEC_FREEVIEW && (!m_apPlayers[pMsg->m_SpectatorID] || m_apPlayers[pMsg->m_SpectatorID]->GetTeam() == TEAM_SPECTATORS))
				SendChatTarget(ClientID, "Invalid spectator id used");
			else
				pPlayer->m_SpectatorID = pMsg->m_SpectatorID;
		}
		else if (MsgID == NETMSGTYPE_CL_CHANGEINFO)
		{
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChangeInfo && pPlayer->m_LastChangeInfo+Server()->TickSpeed()*5 > Server()->Tick())
				return;

			pPlayer->m_LastChangeInfo = Server()->Tick();
			m_pController->OnPlayerInfoChange(pPlayer);

			CNetMsg_Cl_ChangeInfo *pMsg = (CNetMsg_Cl_ChangeInfo *)pRawMsg;
			Server()->SetClientCountry(ClientID, pMsg->m_Country);
			if(Server()->GetItemCount(pPlayer->GetCID(), CUSTOMSKIN))
				str_copy(pPlayer->m_TeeInfos.m_SkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_SkinName));
		}
		else if (MsgID == NETMSGTYPE_CL_EMOTICON && !m_World.m_Paused)
		{
			CNetMsg_Cl_Emoticon *pMsg = (CNetMsg_Cl_Emoticon *)pRawMsg;
			SendEmoticon(ClientID, pMsg->m_Emoticon);

			if(pPlayer->GetCharacter() && pMsg->m_Emoticon >= 2)
				pPlayer->GetCharacter()->ParseEmoticionButton(ClientID, pMsg->m_Emoticon);
		}
		else if (MsgID == NETMSGTYPE_CL_KILL && !m_World.m_Paused)
		{
			if((pPlayer->m_LastKill && pPlayer->m_LastKill+Server()->TickSpeed()*3 > Server()->Tick()) || !Server()->GetItemCount(ClientID, SDROP) || pPlayer->m_Search || pPlayer->m_InArea)
				return;

			pPlayer->m_LastKill = Server()->Tick();
			pPlayer->KillCharacter(WEAPON_SELF);
		}
	}
	else
	{
		if(MsgID == NETMSGTYPE_CL_STARTINFO)
		{
			if(pPlayer->m_IsReady)
				return;

			CNetMsg_Cl_StartInfo *pMsg = (CNetMsg_Cl_StartInfo *)pRawMsg;
			pPlayer->m_LastChangeInfo = Server()->Tick();

			// set start infos
			Server()->SetClientName(ClientID, pMsg->m_pName);
			Server()->SetClientClan(ClientID, pMsg->m_pClan);
			Server()->SetClientCountry(ClientID, pMsg->m_Country);

			str_copy(pPlayer->m_TeeInfos.m_SkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_SkinName));
			pPlayer->m_TeeInfos.m_UseCustomColor = true;
			m_pController->OnPlayerInfoChange(pPlayer);	
			SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你可以用 /lang 命令来改变mod语言."), NULL);

			// send tuning parameters to client
			SendTuningParams(ClientID);

			// client is ready to enter
			pPlayer->m_IsReady = true;
			CNetMsg_Sv_ReadyToEnter m;
			Server()->SendPackMsg(&m, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID);
		}
	}
}

void CGameContext::BuyItem(int ItemType, int ClientID, int Type)
{
	if(!m_apPlayers[ClientID] || !m_apPlayers[ClientID]->GetCharacter()
		|| (m_apPlayers[ClientID]->m_LastChangeInfo && m_apPlayers[ClientID]->m_LastChangeInfo+Server()->TickSpeed() > Server()->Tick()))
		return;
	
	m_apPlayers[ClientID]->m_LastChangeInfo = Server()->Tick();
	if(Server()->GetItemCount(ClientID, ItemType) && ItemType != CLANTICKET && ItemType != BOOKEXPMIN)
		return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经在物品栏了"), NULL);	
	
	if(m_apPlayers[ClientID]->AccData.Level < Server()->GetItemPrice(ClientID, ItemType, 0))
		return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有达到规定的等级"), NULL);	
		
	if(Type == 0 && m_apPlayers[ClientID]->AccData.Gold < Server()->GetItemPrice(ClientID, ItemType, 1))
		return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的黄金,小穷光蛋"), NULL);	

	if(Type == 1 && m_apPlayers[ClientID]->AccData.Donate < Server()->GetItemPrice(ClientID, ItemType, 1))
		return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够点券(donate money),充钱吧"), NULL);	

	if(Type == 0)
	{
		int NeedMaterial = Server()->GetItemPrice(ClientID, ItemType, 1);
		if(Server()->GetMaterials(0) < NeedMaterial)
			return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("店里可没材料."), NULL);	

		Server()->SetMaterials(0, Server()->GetMaterials(0)-NeedMaterial);
	}

	if(Type == 0) m_apPlayers[ClientID]->AccData.Gold -= Server()->GetItemPrice(ClientID, ItemType, 1);
	else if(Type == 1) m_apPlayers[ClientID]->AccData.Donate -= Server()->GetItemPrice(ClientID, ItemType, 1);
	GiveItem(ClientID, ItemType, 1);
	SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你成功地购买了商品"), NULL);							
	
	if(ItemType == IGUN) m_apPlayers[ClientID]->GetCharacter()->GiveWeapon(WEAPON_GUN, 5);
	if(ItemType == ISHOTGUN) m_apPlayers[ClientID]->GetCharacter()->GiveWeapon(WEAPON_SHOTGUN, 5);
	if(ItemType == IGRENADE) m_apPlayers[ClientID]->GetCharacter()->GiveWeapon(WEAPON_GRENADE, 5);
	if(ItemType == ILASER) m_apPlayers[ClientID]->GetCharacter()->GiveWeapon(WEAPON_RIFLE, 5);
	
	dbg_msg("buy/shop", "%s buy item %s:%d", Server()->ClientName(ClientID), Server()->GetItemName(ClientID, ItemType, false), ItemType);
	m_apPlayers[ClientID]->m_LoginSync = 10;
	UpdateStats(ClientID);
	return;	

}

void CGameContext::BuyUpgradeClan(int ClientID, int Money, int Type, const char* SubType)
{
	if(!m_apPlayers[ClientID] || !m_apPlayers[ClientID]->GetCharacter())
		return;
	
	if(Server()->GetLeader(ClientID, Server()->GetClanID(ClientID)) 
		&& Server()->GetClan(DMONEY, Server()->GetClanID(ClientID)) >= Money)
	{
		Server()->InitClanID(Server()->GetClanID(ClientID), MINUS, "Money", Money, true);
		Server()->InitClanID(Server()->GetClanID(ClientID), PLUS, SubType, 1, true);
		SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你购买了公会升级!"), NULL);							
		
		ResetVotes(ClientID, AUTH);
		return;
	}
	else SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你不是会长或者你没有足够的钱"), NULL);								
}

bool CGameContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);

	if(pSelf->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
	else
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
	
	return true;
}

bool CGameContext::ConTuneReset(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CTuningParams TuningParams;
	*pSelf->Tuning() = TuningParams;
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");
	
	return true;
}

bool CGameContext::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	char aBuf[256];
	for(int i = 0; i < pSelf->Tuning()->Num(); i++)
	{
		float v;
		pSelf->Tuning()->Get(i, &v);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pSelf->Tuning()->m_apNames[i], v);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
	
	return true;
}

bool CGameContext::ConPause(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	if(pSelf->m_pController->IsGameOver())
		return true;

	pSelf->m_World.m_Paused ^= 1;
	
	return true;
}

bool CGameContext::ConRestart(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_pController->StartRound();
	
	return true;
} 

bool CGameContext::ConBroadcast(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendBroadcast(-1, pResult->GetString(0), BROADCAST_PRIORITY_SERVERANNOUNCE, pSelf->Server()->TickSpeed()*3);
	
	return true;
}

bool CGameContext::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendChat(-1, CGameContext::CHAT_ALL, pResult->GetString(0));
	
	return true;
}

void CGameContext::CheckPureTuning()
{
	// might not be created yet during start up
	if(!m_pController)
		return;

	if(	str_comp(m_pController->m_pGameType, "DM")==0 ||
		str_comp(m_pController->m_pGameType, "TDM")==0 ||
		str_comp(m_pController->m_pGameType, "CTF")==0)
	{
		CTuningParams p;
		if(mem_comp(&p, &m_Tuning, sizeof(p)) != 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "resetting tuning due to pure server");
			m_Tuning = p;
		}
	}
}

void CGameContext::GiveItem(int ClientID, int ItemID, int Count, int Enchant)
{
	if(ClientID > MAX_NOBOT || ClientID < 0 || !m_apPlayers[ClientID])
		return;

	if((ItemID >= AMULETCLEEVER && ItemID <= APAIN) || (ItemID >= SNAPDAMAGE && ItemID <= FORMULAFORRING) || (ItemID >= RINGBOOMER && ItemID <= EARRINGSKWAH) || (ItemID >= BOOKMONEYMIN && ItemID <= RELRINGS)
		|| (ItemID >=CLANBOXEXP && ItemID <= CUSTOMSKIN) || (ItemID >= WHITETICKET && ItemID <= RANDOMCRAFTITEM) 
		|| (ItemID >=GUNBOUNCE && ItemID <= LAMPHAMMER) || ItemID == JUMPIMPULS || ItemID == FARMBOX || ItemID == PIZDAMET) // rare iteems
	{
		SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 获得了 {str:items}x{int:counts}"), "name", Server()->ClientName(ClientID), "items", Server()->GetItemName(ClientID, ItemID, false), "counts", &Count, NULL);					
		
		if(m_apPlayers[ClientID]->GetCharacter())
			CreateLolText(m_apPlayers[ClientID]->GetCharacter(), false, vec2(0,-75), vec2 (0,-1), 50, Server()->GetItemName(ClientID, ItemID, false));
	}
	if(Server()->GetItemType(ClientID, ItemID) == 10)
	{
		if(ItemID == FARMLEVEL)
			SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("[专长] {str:items}"), "items", Server()->GetItemName(ClientID, ItemID), NULL);				
		else if(ItemID == MINEREXP)
			SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("[专长] {str:items}"), "items", Server()->GetItemName(ClientID, ItemID), NULL);				
		else if(ItemID == LOADEREXP)
			SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("[专长] {str:items}"), "items", Server()->GetItemName(ClientID, ItemID), NULL);				
		else
			SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 获得了 {str:items}"), "name", Server()->ClientName(ClientID), "items", Server()->GetItemName(ClientID, ItemID), NULL);				
	}
	else 
		SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你获得了 {str:items}x{int:counts}"), "items", Server()->GetItemName(ClientID, ItemID), "counts", &Count, NULL);				

	int Settings = 0;
	if(ItemID == COOPERPIX) Settings = 180*Count;
	if(ItemID == IRONPIX) Settings = 211*Count;
	if(ItemID == GOLDPIX) Settings = 491*Count;
	if(ItemID == DIAMONDPIX) Settings = 699*Count;

	Server()->GiveItem(ClientID, ItemID, Count, Settings, Enchant);
}

void CGameContext::CreateItem(int ClientID, int ItemID, int Count)
{
	if(!m_apPlayers[ClientID] || !m_apPlayers[ClientID]->GetCharacter())
		return;
	
	if(m_apPlayers[ClientID]->m_LastChangeInfo && m_apPlayers[ClientID]->m_LastChangeInfo+Server()->TickSpeed()*3 > Server()->Tick())
		return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("请稍候..."), NULL);

	m_apPlayers[ClientID]->m_LastChangeInfo = Server()->Tick();

	switch(ItemID)
	{
		default: SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("合成错误"), NULL); break;
		case RARERINGSLIME:
		{
			if(!Server()->GetItemCount(ClientID, RARESLIMEDIRT) || !Server()->GetItemCount(ClientID, FORMULAFORRING))
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "戒指蓝图, Slime Dirt", NULL);

			Server()->RemItem(ClientID, RARESLIMEDIRT, Count, -1);
			Server()->RemItem(ClientID, FORMULAFORRING, Count, -1);	
		} break;
		case MODULEEMOTE: 
		{	
			if(!Server()->GetItemCount(ClientID, AHAPPY) || !Server()->GetItemCount(ClientID, AEVIL) || 
				!Server()->GetItemCount(ClientID, ASUPRRISE) || !Server()->GetItemCount(ClientID, ABLINK) || !Server()->GetItemCount(ClientID, APAIN))
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "眼睛表情 (happy, evil, surprise, blink, pain)", NULL);

			Server()->RemItem(ClientID, AHAPPY, Count, -1);
			Server()->RemItem(ClientID, AEVIL, Count, -1);
			Server()->RemItem(ClientID, ASUPRRISE, Count, -1);
			Server()->RemItem(ClientID, ABLINK, Count, -1);
			Server()->RemItem(ClientID, APAIN, Count, -1);
			Server()->RemItem(ClientID, APAIN, Count, -1);
		} break;
		case WEAPONPRESSED: 
		{
			if(!Server()->GetItemCount(ClientID, IGUN) || !Server()->GetItemCount(ClientID, ISHOTGUN) || 
				!Server()->GetItemCount(ClientID, IGRENADE) || !Server()->GetItemCount(ClientID, ILASER))
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "武器 (手枪, 散弹枪, 榴弹炮, 激光枪)", NULL);

			Server()->RemItem(ClientID, IGUN, Count, -1);
			Server()->RemItem(ClientID, ISHOTGUN, Count, -1);
			Server()->RemItem(ClientID, IGRENADE, Count, -1);
			Server()->RemItem(ClientID, ILASER, Count, -1);
		} break;
		case RINGBOOMER: 
		{
			if(!Server()->GetItemCount(ClientID, FORMULAFORRING) || Server()->GetItemCount(ClientID, HEADBOOMER) < 100)
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "戒指蓝图, 爆破鬼才的尸体 x100", NULL);

			Server()->RemItem(ClientID, HEADBOOMER, 100, -1);
			Server()->RemItem(ClientID, FORMULAFORRING, Count, -1);	
		} break;
		case MODULESHOTGUNSLIME: 
		{
			if(!Server()->GetItemCount(ClientID, FORMULAWEAPON) || !Server()->GetItemCount(ClientID, RINGBOOMER))
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "武器蓝图, 爆破鬼才的戒指", NULL);

			Server()->RemItem(ClientID, FORMULAWEAPON, Count, -1);
			Server()->RemItem(ClientID, RINGBOOMER, Count, -1);
		} break;
		case EARRINGSKWAH: 
		{
			if(!Server()->GetItemCount(ClientID, FORMULAEARRINGS) || Server()->GetItemCount(ClientID, FOOTKWAH) < 100)
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "耳环蓝图, Kwah 脚x100", NULL);

			Server()->RemItem(ClientID, FORMULAEARRINGS, Count, -1);
			Server()->RemItem(ClientID, FOOTKWAH, 100, -1);
		} break;
		case ZOMIBEBIGEYE: 
		{
			if(Server()->GetItemCount(ClientID, ZOMBIEEYE) < 30)
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "僵尸眼x30", NULL);

			Server()->RemItem(ClientID, ZOMBIEEYE, 30, -1);
		} break;
		case SKELETSSBONE: 
		{
			if(Server()->GetItemCount(ClientID, SKELETSBONE) < 30)
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "骷髅骨头x30", NULL);

			Server()->RemItem(ClientID, SKELETSBONE, 30, -1);
		} break;
		case CUSTOMSKIN: 
		{
			if(Server()->GetItemCount(ClientID, SKELETSSBONE) < 30 || Server()->GetItemCount(ClientID, ZOMIBEBIGEYE) < 30)
				return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "骷髅强化骨x30, 僵尸大眼睛x30", NULL);

			Server()->RemItem(ClientID, SKELETSSBONE, 30, -1);
			Server()->RemItem(ClientID, ZOMIBEBIGEYE, 30, -1);
		} break;
		case ENDEXPLOSION: 
		{
			if(!Server()->GetItemCount(ClientID, BIGCRAFT))
				GiveItem(ClientID, BIGCRAFT, 1);

			if(Server()->GetItemCount(ClientID, FORMULAWEAPON) < 25)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "秘密武器x25", NULL);
				return;
			}

			Server()->RemItem(ClientID, FORMULAWEAPON, 25, -1);
		} break;
		case SHEALSUMMER: 
		{
			if(Server()->GetItemCount(ClientID, ESUMMER) < 20)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "日耀x20", NULL);
				return;
			}
			Server()->RemItem(ClientID, ESUMMER, 20, -1);
			if(rand()%100 < 96)
			{
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 在合成 {str:item}x{int:coun} 的时候失败了"), "name", Server()->ClientName(ClientID), "item", Server()->GetItemName(ClientID, ItemID, false), "coun", &Count ,NULL);				
				return;
			}
			if(!Server()->GetItemCount(ClientID, TITLESUMMER))
				GiveItem(ClientID, TITLESUMMER, 1);

		} break;
		case JUMPIMPULS: 
		{
			if(Server()->GetItemCount(ClientID, TOMATE) < 60 || Server()->GetItemCount(ClientID, POTATO) < 60 || Server()->GetItemCount(ClientID, CARROT) < 60)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "(土豆番茄萝卜)x60", NULL);
				return;
			}
			Server()->RemItem(ClientID, TOMATE, 30, -1);
			Server()->RemItem(ClientID, POTATO, 30, -1);
			Server()->RemItem(ClientID, CARROT, 30, -1);
		} break;

		case COOPERPIX: 
		{
			if(Server()->GetItemCount(ClientID, WOOD) < 30 || Server()->GetItemCount(ClientID, COOPERORE) < 60)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "木头x30, 铜矿x60", NULL);
				return;
			}
			Server()->RemItem(ClientID, WOOD, 30, -1);
			Server()->RemItem(ClientID, COOPERORE, 60, -1);
		} break;
		case IRONPIX: 
		{
			if(Server()->GetItemCount(ClientID, WOOD) < 40 || Server()->GetItemCount(ClientID, IRONORE) < 60)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "木头x40, 铁矿x60", NULL);
				return;
			}
			Server()->RemItem(ClientID, WOOD, 40, -1);
			Server()->RemItem(ClientID, IRONORE, 60, -1);
		} break;
		case GOLDPIX: 
		{
			if(Server()->GetItemCount(ClientID, WOOD) < 50 || Server()->GetItemCount(ClientID, GOLDORE) < 80)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "木头x50, 金矿x80", NULL);
				return;
			}
			Server()->RemItem(ClientID, WOOD, 50, -1);
			Server()->RemItem(ClientID, GOLDORE, 80, -1);
		} break;
		case DIAMONDPIX: 
		{
			if(Server()->GetItemCount(ClientID, WOOD) < 50 || Server()->GetItemCount(ClientID, DIAMONDORE) < 100)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "木头x50, 钻石矿x100", NULL);
				return;
			}
			Server()->RemItem(ClientID, WOOD, 50, -1);
			Server()->RemItem(ClientID, DIAMONDORE, 100, -1);
		} break;
		case FORMULAEARRINGS: 
		{
			if(Server()->GetItemCount(ClientID, IRONORE) < 100 || Server()->GetItemCount(ClientID, COOPERORE) < 100)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "(铁矿, 铜矿)x100", NULL);
				return;
			}
			Server()->RemItem(ClientID, COOPERORE, 100, -1);
			Server()->RemItem(ClientID, IRONORE, 100, -1);
		} break;
		case FORMULAFORRING: 
		{
			if(Server()->GetItemCount(ClientID, IRONORE) < 125 || Server()->GetItemCount(ClientID, COOPERORE) < 125)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "(铁矿, 铜矿)x125", NULL);
				return;
			}
			Server()->RemItem(ClientID, COOPERORE, 125, -1);
			Server()->RemItem(ClientID, IRONORE, 125, -1);
		} break;
		case FORMULAWEAPON: 
		{
			if(Server()->GetItemCount(ClientID, IRONORE) < 150 || Server()->GetItemCount(ClientID, COOPERORE) < 150)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "(铁矿, 铜矿)x150", NULL);
				return;
			}
			Server()->RemItem(ClientID, COOPERORE, 150, -1);
			Server()->RemItem(ClientID, IRONORE, 150, -1);
		} break;
		case LEATHERBODY: 
		{
			if(Server()->GetItemCount(ClientID, LEATHER) < 50 || Server()->GetItemCount(ClientID, WOOD) < 150)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "皮革x50, 木头x150", NULL);
				return;
			}
			Server()->RemItem(ClientID, LEATHER, 50, -1);
			Server()->RemItem(ClientID, WOOD, 150, -1);
		} break;
		case LEATHERFEET: 
		{
			if(Server()->GetItemCount(ClientID, LEATHER) < 40 || Server()->GetItemCount(ClientID, WOOD) < 120)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "皮革x40, 木头x120", NULL);
				return;
			}
			Server()->RemItem(ClientID, LEATHER, 40, -1);
			Server()->RemItem(ClientID, WOOD, 120, -1);
		} break;
		case COOPERBODY: 
		{
			if(Server()->GetItemCount(ClientID, COOPERORE) < 500 || Server()->GetItemCount(ClientID, WOOD) < 150)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "铜矿x500, 木头x150", NULL);
				return;
			}
			Server()->RemItem(ClientID, COOPERORE, 500, -1);
			Server()->RemItem(ClientID, WOOD, 150, -1);
		} break;
		case COOPERFEET: 
		{
			if(Server()->GetItemCount(ClientID, COOPERORE) < 400 || Server()->GetItemCount(ClientID, WOOD) < 120)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "铜矿x400, 木头x120", NULL);
				return;
			}
			Server()->RemItem(ClientID, COOPERORE, 400, -1);
			Server()->RemItem(ClientID, WOOD, 120, -1);
		} break;
		case IRONBODY: 
		{
			if(Server()->GetItemCount(ClientID, IRONORE) < 500 || Server()->GetItemCount(ClientID, WOOD) < 150)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "铁矿x500, 木头x150", NULL);
				return;
			}
			Server()->RemItem(ClientID, IRONORE, 500, -1);
			Server()->RemItem(ClientID, WOOD, 150, -1);
		} break;
		case IRONFEET: 
		{
			if(Server()->GetItemCount(ClientID, IRONORE) < 400 || Server()->GetItemCount(ClientID, WOOD) < 120)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "铁矿x400, 木头x120", NULL);
				return;
			}
			Server()->RemItem(ClientID, IRONORE, 400, -1);
			Server()->RemItem(ClientID, WOOD, 120, -1);
		} break;
		case GOLDBODY: 
		{
			if(Server()->GetItemCount(ClientID, GOLDORE) < 500 || Server()->GetItemCount(ClientID, WOOD) < 150)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "金矿x500, 木头x150", NULL);
				return;
			}
			Server()->RemItem(ClientID, GOLDORE, 500, -1);
			Server()->RemItem(ClientID, WOOD, 150, -1);
		} break;
		case GOLDFEET: 
		{
			if(Server()->GetItemCount(ClientID, GOLDORE) < 400 || Server()->GetItemCount(ClientID, WOOD) < 120)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "金矿x400, 木头x120", NULL);
				return;
			}
			Server()->RemItem(ClientID, GOLDORE, 400, -1);
			Server()->RemItem(ClientID, WOOD, 120, -1);
		} break;
		case DIAMONDBODY: 
		{
			if(Server()->GetItemCount(ClientID, DIAMONDORE) < 500 || Server()->GetItemCount(ClientID, WOOD) < 150)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "钻石矿x500, 木头x150", NULL);
				return;
			}
			Server()->RemItem(ClientID, DIAMONDORE, 500, -1);
			Server()->RemItem(ClientID, WOOD, 150, -1);
		} break;
		case DIAMONDFEET: 
		{
			if(Server()->GetItemCount(ClientID, DIAMONDORE) < 400 || Server()->GetItemCount(ClientID, WOOD) < 120)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "钻石矿x400, 木头x120", NULL);
				return;
			}
			Server()->RemItem(ClientID, DIAMONDORE, 400, -1);
			Server()->RemItem(ClientID, WOOD, 120, -1);
		} break;
		case DRAGONBODY: 
		{
			if(Server()->GetItemCount(ClientID, DRAGONORE) < 500 || Server()->GetItemCount(ClientID, WOOD) < 150)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "龙矿x500, 木头x150", NULL);
				return;
			}
			Server()->RemItem(ClientID, DRAGONORE, 500, -1);
			Server()->RemItem(ClientID, WOOD, 150, -1);
		} break;
		case DRAGONFEET: 
		{
			if(Server()->GetItemCount(ClientID, DRAGONORE) < 400 || Server()->GetItemCount(ClientID, WOOD) < 120)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "龙矿x400, 木头x120", NULL);
				return;
			}
			Server()->RemItem(ClientID, DRAGONORE, 400, -1);
			Server()->RemItem(ClientID, WOOD, 120, -1);
		} break;
		case STCLASIC: 
		{
			if(Server()->GetItemCount(ClientID, COOPERORE) < 100 || Server()->GetItemCount(ClientID, IRONORE) < 10)
			{
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("为了合成你需要 {str:need}"), "need", "铜矿x100, 铁矿x10", NULL);
				return;
			}
			Server()->RemItem(ClientID, COOPERORE, 100, -1);
			Server()->RemItem(ClientID, IRONORE, 10, -1);
		} break;
	}
	SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 合成了物品 {str:item}x{int:coun}"), "name", Server()->ClientName(ClientID), "item", Server()->GetItemName(ClientID, ItemID, false), "coun", &Count ,NULL);				
	SendMail(ClientID, "Hello, 合成物品成功!", ItemID, Count);
}

void CGameContext::BuySkill(int ClientID, int Price, int ItemID)
{
	if(Server()->GetItemCount(ClientID, ItemID))
		return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你已经升到满级了"), NULL);	 

	if(m_apPlayers[ClientID]->AccUpgrade.SkillPoint < Price)
		return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你没有足够的技能点"), NULL);	

	SendMail(ClientID, "你解锁了一项新技能!", ItemID, 1);
	m_apPlayers[ClientID]->AccUpgrade.SkillPoint -= Price;
	SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你的技能成功地提升了"), NULL);							
		
	UpdateUpgrades(ClientID);
	ResetVotes(ClientID, AUTH);	
	return;
}


void CGameContext::SkillSettings(int ClientID, int ItemType, const char *Msg)
{
	if(Server()->GetItemCount(ClientID, ItemType))
	{
		const char *Data = "F4"; 
		switch(Server()->GetItemSettings(ClientID, ItemType))
		{
			case 1: Data = "F3"; break;
			case 2: Data = "EMOTE HEARTH"; break;
			case 3: Data = "EMOTE PAIN"; break;
			case 4: Data = "EMOTE ..."; break;
			case 5: Data = "EMOTE MUSIC"; break;
			case 6: Data = "EMOTE SORRY"; break;
			case 7: Data = "EMOTE GHOST"; break;
		}
		AddVote_Localization(ClientID, Msg, "Settings {str:stat} {str:name}", "stat", Data, "name", Server()->GetItemName(ClientID, ItemType));
	}
}

void CGameContext::ResetVotes(int ClientID, int Type)
{	
	if(!m_apPlayers[ClientID])
		return;
	
	if(m_apPlayers[ClientID]->IsBot())
		return;
		
	ClearVotes(ClientID);
			
	if(m_PlayerVotes[ClientID].size())
		return;

	if(m_apPlayers[ClientID]->GetCharacter() && Type > AUTH)
		CreateSound(m_apPlayers[ClientID]->GetCharacter()->m_Pos, 25);
	
	// ############################### Основное не авторизированных
	if(Type == NOAUTH)
	{
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "info", "制作者名单");
		AddVote_Localization(ClientID, "help", "游戏如何开始?");
		AddVote_Localization(ClientID, "null", "- - - - - ");
		AddVote_Localization(ClientID, "null", "rAzataz 修改 infClass 通过 Kurosio");
		AddVote_Localization(ClientID, "null", "所有制作者信息在 '作者信息' 内");
		return;
	}
	
	// ############################### Основное меню авторизированных
	else if(Type == AUTH)
	{
		if(m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
			return;

		if(Server()->GetClanID(ClientID) > 0)
			Server()->UpdClanCount(Server()->GetClanID(ClientID));

		AddVote_Localization(ClientID, "null", "☪ 账户: {str:Username}", "Username", Server()->ClientUsername(ClientID));
		AddVote_Localization(ClientID, "null", "ღ 等级: {int:Level} / 经验: {int:Exp}", "Level", &m_apPlayers[ClientID]->AccData.Level, "Exp", &m_apPlayers[ClientID]->AccData.Exp);
		AddVote_Localization(ClientID, "null", "ღ 黄金: {int:gold} 白银: {int:Money}", "gold", &m_apPlayers[ClientID]->AccData.Gold ,"Money", &m_apPlayers[ClientID]->AccData.Money);
		AddVote("······················· ", "null", ClientID);
		AddVote_Localization(ClientID, "null", "# {str:psevdo}", "psevdo", LocalizeText(ClientID, "子菜单--信息"));
		AddVote_Localization(ClientID, "info", "☞ {str:dis}", "dis", g_Config.m_Discord);
		AddVote_Localization(ClientID, "rrul", "☞ 注意事项");
		AddVoteMenu_Localization(ClientID, RESLIST, MENUONLY, "☞ 通缉犯");
		AddVoteMenu_Localization(ClientID, EVENTLIST, MENUONLY, "☞ 事件与奖金");
		AddVoteMenu_Localization(ClientID, JOBSSET, MENUONLY, "☞ 工作与专长");
		AddVoteMenu_Localization(ClientID, TOPMENU, MENUONLY, "☞ 玩家/公会排名");

		AddVote("······················· ", "null", ClientID);
		AddVote_Localization(ClientID, "null", "♫ {str:psevdo}", "psevdo", LocalizeText(ClientID, "子菜单--账户"));
		AddVoteMenu_Localization(ClientID, MAILMENU, MENUONLY, "☞ 邮箱 ✉");	
		AddVoteMenu_Localization(ClientID, ARMORMENU, MENUONLY, "☞ 装备 ☭");	
		AddVoteMenu_Localization(ClientID, INVENTORY, MENUONLY, "☞ 物品栏/背包 ✪");		
		AddVoteMenu_Localization(ClientID, CRAFTING, MENUONLY, "☞ 合成栏 ☺");
		AddVoteMenu_Localization(ClientID, QUEST, MENUONLY, "☞ 任务与报酬 ⊹");

		AddVote("······················· ", "null", ClientID);
		AddVote_Localization(ClientID, "null", "✪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "子菜单--设置"));
		AddVoteMenu_Localization(ClientID, CLMENU, MENUONLY, "☞ 升级与职业 [♣{str:class}♣]", "class", m_apPlayers[ClientID]->GetClassName());
		AddVoteMenu_Localization(ClientID, SETTINGS, MENUONLY, "☞ 设置与安全");
		AddVoteMenu_Localization(ClientID, CDONATE, MENUONLY, "☞ 充钱与特权");
		AddVoteMenu_Localization(ClientID, INTITLE, MENUONLY, "☞ 成就与称号");

		if(Server()->GetClanID(ClientID) > 0)
			AddVoteMenu_Localization(ClientID, CLAN, MENUONLY, "☞ 公会菜单 {str:clan}", "clan", Server()->ClientClan(ClientID));

		if(m_apPlayers[ClientID]->GetShop())
		{
			if(Server()->GetItemCount(ClientID, MATERIAL))
			{
				int Count = Server()->GetItemCount(ClientID, MATERIAL);
				int Gold = Count/5;
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("所需材料 {int:count} : 而你有 {int:money} 黄金"), 
					"count", &Count, "money", &Gold, NULL);	

				Server()->SetMaterials(0, Server()->GetMaterials(0)+Count);
				Server()->RemItem(ClientID, MATERIAL, Count, -1);
				m_apPlayers[ClientID]->AccData.Gold += Gold;
			}

			AddVote("", "null", ClientID);
				
			// ##################### ПРЕДМЕТЫ
			AddVote_Localization(ClientID, "null", "✄  {str:psevdo}", "psevdo", LocalizeText(ClientID, "物品"));
			
			// WEAPON GUN
			CreateNewShop(ClientID, IGUN, 3, 3, 40);
			CreateNewShop(ClientID, IGRENADE, 3, 8, 70);
			CreateNewShop(ClientID, ISHOTGUN, 3, 15, 120);
			CreateNewShop(ClientID, ILASER, 3, 16, 310);
			CreateNewShop(ClientID, HYBRIDSG, 3, 120, 40000);
			AddVote("············", "null", ClientID);
			
			// #################### УЛУЧШЕНИЯ
			AddVote_Localization(ClientID, "null", "★  {str:psevdo}", "psevdo", LocalizeText(ClientID, "升级"));
			
			CreateNewShop(ClientID, HOOKDAMAGE, 3, 18, 100);
			CreateNewShop(ClientID, GUNAUTO, 3, 20, 150);
			CreateNewShop(ClientID, HAMMERAUTO, 3, 20, 1200);

			CreateNewShop(ClientID, EXGUN, 3, 25, 1800);
			CreateNewShop(ClientID, EXSHOTGUN, 3, 30, 10000);
			CreateNewShop(ClientID, EXLASER, 3, 30, 1000);
			CreateNewShop(ClientID, MODULEHOOKEXPLODE, 3, 40, 4000);
			
			CreateNewShop(ClientID, GHOSTGUN, 3, 60, 3000);
			CreateNewShop(ClientID, GHOSTSHOTGUN, 3, 70, 15000);
			CreateNewShop(ClientID, GHOSTGRENADE, 3, 70, 15000);
			CreateNewShop(ClientID, PIZDAMET, 3, 90, 15000);

			CreateNewShop(ClientID, GUNBOUNCE, 3, 90, 20000);
			CreateNewShop(ClientID, GRENADEBOUNCE, 3, 90, 20000);
		
			CreateNewShop(ClientID, LAMPHAMMER, 3, 160, 50000);

			// #################### СКИЛЫ
			AddVote("············", "null", ClientID);
			AddVote_Localization(ClientID, "null", "ღ  {str:psevdo}", "psevdo", LocalizeText(ClientID, "技巧"));
			AddVote_Localization(ClientID, "null", "没有物品");
			
			// #################### ПЕРСОНАЛЬНО
			
			AddVote("············", "null", ClientID);
			AddVote_Localization(ClientID, "null", "♫  {str:psevdo}", "psevdo", LocalizeText(ClientID, "个人"));
			CreateNewShop(ClientID, RESETINGUPGRADE, 3, 1, 30000);
			CreateNewShop(ClientID, RESETINGSKILL, 3, 1, 8000);
			CreateNewShop(ClientID, WHITETICKET, 3, 100, 20000);
			CreateNewShop(ClientID, MOONO2, 3, 100, 5);
			CreateNewShop(ClientID, BOOKEXPMIN, 2, 1, 100);
			CreateNewShop(ClientID, CLANTICKET, 2, 15, 2500);

			// #################### РЕДКИЕ
			
			AddVote("············", "null", ClientID);
			AddVote_Localization(ClientID, "null", "  {str:psevdo}", "psevdo", LocalizeText(ClientID, "稀有物品"));
			CreateNewShop(ClientID, AMULETCLEEVER, 3, 1, 1200);
			AddVote_Localization(ClientID, "null", "这个可以使你在升级的时候获得两个钱袋");
			CreateNewShop(ClientID, RINGNOSELFDMG, 3, 1, 1000);
			AddVote_Localization(ClientID, "null", "这个可以使你避免自己伤到自己");
			AddVote("", "null", ClientID);
		}

		if(m_apPlayers[ClientID]->GetWork())
		{
			AddVote("", "null", ClientID);
			AddVote_Localization(ClientID, "null", "在投票的理由填写处填写出售的个数");
			CreateSellWorkItem(ClientID, COOPERORE, 1);
			CreateSellWorkItem(ClientID, IRONORE, 2);
			CreateSellWorkItem(ClientID, GOLDORE, 2);
			CreateSellWorkItem(ClientID, DIAMONDORE, 3);
			CreateSellWorkItem(ClientID, DRAGONORE, 10);
		}
		return;
	}
	
	else if(Type == JOBSSET)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "工作与专长");
		AddVote("", "null", ClientID);
		int Level = 1+Server()->GetItemCount(ClientID, FARMLEVEL)/g_Config.m_SvFarmExp;
		int NeedExp = Level*g_Config.m_SvFarmExp;
		int Exp = Server()->GetItemCount(ClientID, FARMLEVEL);
		AddVote_Localization(ClientID, "null", "专长 种地 ({int:exp}/{int:nexp} 等级: {int:lvl})", "exp", &Exp, "nexp", &NeedExp, "lvl", &Level);
	
		Level = 1+Server()->GetItemCount(ClientID, MINEREXP)/g_Config.m_SvMinerExp;
		NeedExp = Level*g_Config.m_SvMinerExp;
		Exp = Server()->GetItemCount(ClientID, MINEREXP);
		AddVote_Localization(ClientID, "null", "专长 挖土 ({int:exp}/{int:nexp} 等级: {int:lvl})", "exp", &Exp, "nexp", &NeedExp, "lvl", &Level);

		Level = 1+Server()->GetItemCount(ClientID, LOADEREXP)/g_Config.m_SvMaterExp;
		NeedExp = Level*g_Config.m_SvMaterExp;
		Exp = Server()->GetItemCount(ClientID, LOADEREXP);
		AddVote_Localization(ClientID, "null", "专长 萃取 ({int:exp}/{int:nexp} 等级: {int:lvl})", "exp", &Exp, "nexp", &NeedExp, "lvl", &Level);

		AddVote_Localization(ClientID, "null", "专长 伐木工 (光头强不会升级)");
		AddVote_Localization(ClientID, "null", "专长 摸鱼 (敬请期待)");

		AddBack(ClientID);
		return;
	}

	else if(Type == MAILMENU)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "我的邮箱");
		Server()->InitMailID(ClientID);
		AddBack(ClientID);
		AddVote("", "null", ClientID);
	}


	else if(Type == ARMORMENU) 
	{
		m_apPlayers[ClientID]->m_UpdateMenu = Type; 
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "我的装备");
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☄ {str:psevdo}", "psevdo", LocalizeText(ClientID, "装备"));
 
		int Bonus = Server()->GetBonusEnchant(ClientID, Server()->GetItemEnquip(ClientID, 15), 15);
		AddVote_Localization(ClientID, "null", "◈ 胸甲 {str:enquip} / 生命值 +{int:hp} 装甲值 +{int:arm}", "enquip", Server()->GetItemName(ClientID, Server()->GetItemEnquip(ClientID, 15)), "hp", &Bonus, "arm", &Bonus);
		Bonus = Server()->GetBonusEnchant(ClientID, Server()->GetItemEnquip(ClientID, 16), 16);
		AddVote_Localization(ClientID, "null", "◈ 靴子 {str:enquip} / 生命值 +{int:hp} 装甲值 +{int:arm}", "enquip", Server()->GetItemName(ClientID, Server()->GetItemEnquip(ClientID, 16)), "hp", &Bonus, "arm", &Bonus);
		Bonus = Server()->GetBonusEnchant(ClientID, Server()->GetItemEnquip(ClientID, 17), 17);
		AddVote_Localization(ClientID, "null", "◈ Stabilized {str:enquip} / 伤害 +{int:dmg}", "enquip", Server()->GetItemName(ClientID, Server()->GetItemEnquip(ClientID, 17)), "dmg", &Bonus);
	

		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☄ {str:psevdo}", "psevdo", LocalizeText(ClientID, "排序与选择"));
		AddVote_Localization(ClientID, "armor1", "☞ 胸甲 {str:enquip}", "enquip", Server()->GetItemName(ClientID, Server()->GetItemEnquip(ClientID, 15)));
		AddVote_Localization(ClientID, "armor2", "☞ 靴子 {str:enquip}", "enquip", Server()->GetItemName(ClientID, Server()->GetItemEnquip(ClientID, 16)));
		AddVote_Localization(ClientID, "armor3", "☞ Stabilized {str:enquip}", "enquip", Server()->GetItemName(ClientID, Server()->GetItemEnquip(ClientID, 17)));

		AddBack(ClientID);
		AddVote("", "null", ClientID);
		
		if(m_apPlayers[ClientID]->m_SelectArmor == 1)
			Server()->ListInventory(ClientID, 15);
		else if(m_apPlayers[ClientID]->m_SelectArmor == 2)
			Server()->ListInventory(ClientID, 16);
		else if(m_apPlayers[ClientID]->m_SelectArmor == 3)
			Server()->ListInventory(ClientID, 17);
	}


	// ############################### Ачивки и Титулы - 成就与称号还有头衔之类的
	else if(Type == INTITLE)
	{
		m_apPlayers[ClientID]->m_UpdateMenu = Type;
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "成就与称号");
		AddVote_Localization(ClientID, "null", "你可以使用称号");
		AddVote_Localization(ClientID, "null", "最后显示更多的称号");
		AddVote("", "null", ClientID);
		CreateNewShop(ClientID, BIGCRAFT, 1, 0, 0);
		CreateNewShop(ClientID, PIGPIG, 1, 0, 0);
		CreateNewShop(ClientID, BOSSDIE, 1, 0, 0);
		CreateNewShop(ClientID, TITLESUMMER, 1, 0, 0);
		CreateNewShop(ClientID, TITLEQUESTS, 1, 0, 0);
		CreateNewShop(ClientID, X2MONEYEXPVIP, 1, 0, 0);
		CreateNewShop(ClientID, TITLEENCHANT, 1, 0, 0);
		AddBack(ClientID);
		return;
	}
	
	// ############################### Меню настроек - 设置菜单
	else if(Type == SETTINGS)
	{
		m_apPlayers[ClientID]->m_UpdateMenu = Type;
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "这是动态的设置");
		AddVote_Localization(ClientID, "null", "是提供给私人的升级的");
		AddVote_Localization(ClientID, "null", "当你购买物品后需要到这里来开启");
		AddVote("", "null", ClientID);

		AddVote_Localization(ClientID, "null", "☪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "修改"));
		if(Server()->GetItemCount(ClientID, SNAPDAMAGE))
			CreateNewShop(ClientID, RAREEVENTHAMMER, 1, 0, 0);

		CreateNewShop(ClientID, JUMPIMPULS, 1, 0, 0);
		CreateNewShop(ClientID, ENDEXPLOSION, 1, 0, 0);
		CreateNewShop(ClientID, HOOKDAMAGE, 1, 0, 0);
		CreateNewShop(ClientID, MODULEHOOKEXPLODE, 1, 0, 0);
		CreateNewShop(ClientID, RINGNOSELFDMG, 1, 0, 0);
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "锤子"));
		CreateNewShop(ClientID, HAMMERAUTO, 1, 0, 0);
		CreateNewShop(ClientID, LAMPHAMMER, 1, 0, 0);
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "手枪"));
		CreateNewShop(ClientID, GUNAUTO, 1, 0, 0);
		CreateNewShop(ClientID, GUNBOUNCE, 1, 0, 0);
		CreateNewShop(ClientID, GHOSTGUN, 1, 0, 0);
		CreateNewShop(ClientID, EXGUN, 1, 0, 0);
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "散弹枪"));
		CreateNewShop(ClientID, HYBRIDSG, 1, 0, 0);
		CreateNewShop(ClientID, GHOSTSHOTGUN, 1, 0, 0);
		CreateNewShop(ClientID, MODULESHOTGUNSLIME, 1, 0, 0);
		CreateNewShop(ClientID, EXSHOTGUN, 1, 0, 0);
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "榴弹炮"));
		CreateNewShop(ClientID, GRENADEBOUNCE, 1, 0, 0);
		CreateNewShop(ClientID, GHOSTGRENADE, 1, 0, 0);
		CreateNewShop(ClientID, PIZDAMET, 1, 0, 0);
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "激光枪"));
		CreateNewShop(ClientID, EXLASER, 1, 0, 0);

		AddVote("························", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☪ {str:psevdo}", "psevdo", LocalizeText(ClientID, "设置"));
		const char *Data = Server()->GetClientAntiPing(ClientID) ? "☑" : "☐";
		AddVote_Localization(ClientID, "ssantiping", "☞ Anti Ping {str:stat}", "stat", Data);

		Data = Server()->GetSeccurity(ClientID) ? "☑" : "☐";
		AddVote_Localization(ClientID, "ssseccurity", "☞ 登录与密码 {str:stat}", "stat", Data);

		Data = Server()->GetItemSettings(ClientID, SANTIPVP) ? "☑" : "☐";
		AddVote_Localization(ClientID, "ssantipvp", "☞ VIP特权: 禁止PVP {str:stat}", "stat", Data);

		Data = "FULL";
		if(Server()->GetItemSettings(ClientID, SCHAT) == 1) Data = "NORMAL";
		else if(Server()->GetItemSettings(ClientID, SCHAT) == 2) Data = "MINIMAL";
		AddVote_Localization(ClientID, "sssetingschat", "☞ 聊天栏输出 ({str:stat})", "stat", Data);

		Data = "HAMMER";
		if(Server()->GetItemSettings(ClientID, SDROP)) Data = "F3";
		AddVote_Localization(ClientID, "sssetingsdrop", "☞ 拾取物品方法 ({str:stat})", "stat", Data);

		AddBack(ClientID);
		return;
	}
	
	// ############################### Меню хилера
	else if(Type == CLMENU)
	{
		m_apPlayers[ClientID]->m_UpdateMenu = Type;
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "这是升级职业的");
		AddVote_Localization(ClientID, "null", "被动技能与主动技能");		
		AddVote_Localization(ClientID, "null", "在投票的理由填写处填写升级数");		
		AddVote_Localization(ClientID, "null", "C - 职业点数.");		
		AddVote_Localization(ClientID, "null", "SP - 需要技能点.");	
		AddVote_Localization(ClientID, "null", "UP - 需要升级点.");	
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "统计数据(升级点 - {int:up} / 技能点 - {int:sp})", "up", &m_apPlayers[ClientID]->AccUpgrade.Upgrade, "sp", &m_apPlayers[ClientID]->AccUpgrade.SkillPoint);
		AddVote("············", "null", ClientID);
		AddVote_Localization(ClientID, "null", "♛ {str:psevdo}", "psevdo", LocalizeText(ClientID, "升级选项"));
		AddVote_Localization(ClientID, "uhealth", "☞ [{int:sum}] 生命值上限 +40({str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.Health, "bonus", m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER ? "C+10" : "C+0");
		AddVote_Localization(ClientID, "udamage", "☞ [{int:sum}] 伤害 +1({str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.Damage, "bonus", "C+0");
		AddVote_Localization(ClientID, "uammoregen", "☞ [{int:sum}] 子弹回复速度 +1({str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.AmmoRegen, "bonus", "C+0");
		AddVote_Localization(ClientID, "uammo", "☞ [{int:sum}] 子弹 +1(UP5 {str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.Ammo, "bonus", "C+0");
		AddVote_Localization(ClientID, "uhpregen", "☞ [{int:sum}] 生命恢复速度 +1({str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.HPRegen, "bonus", "C+0");
		AddVote_Localization(ClientID, "uhandle", "☞ [{int:sum}] 射速 +1({str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.Speed, "bonus", "C+0");
		AddVote_Localization(ClientID, "umana", "☞ [{int:sum}] 魔能 +1({str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.Mana, "bonus", "C+0");
		AddVote_Localization(ClientID, "uspray", "☞ [{int:sum}] 子弹散射 +1(UP10 {str:bonus})", "sum", &m_apPlayers[ClientID]->AccUpgrade.Spray, "bonus", "C+0");
		AddVote("············", "null", ClientID);
		AddVote_Localization(ClientID, "null", "♞ {str:psevdo}", "psevdo", LocalizeText(ClientID, "Passive Skills"));
		
		int Need = HAMMERRANGE;
		if(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_HEALER)
		{
			AddVote_Localization(ClientID, "ushammerrange", "☞ ({int:need}SP) 生命值 +4% ({str:act}) ({int:sum})", "need", &Need, "act", 
				m_apPlayers[ClientID]->AccUpgrade.HammerRange ? "✔" : "x", "sum", &m_apPlayers[ClientID]->AccUpgrade.HammerRange);		
			AddVote_Localization(ClientID, "upasive2", "☞ ({int:need}SP) 抵抗伤害 +2% ({str:act}) ({int:sum})", "need", &Need, "act", 
				m_apPlayers[ClientID]->AccUpgrade.Pasive2 ? "✔" : "x", "sum", &m_apPlayers[ClientID]->AccUpgrade.Pasive2);			
		}
		else if(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_ASSASINS)
		{
			AddVote_Localization(ClientID, "ushammerrange", "☞ ({int:need}SP) 暴击率 +6.67% ({str:act}) ({int:sum})", "need", &Need, "act", 
				m_apPlayers[ClientID]->AccUpgrade.HammerRange ? "✔" : "x", "sum", &m_apPlayers[ClientID]->AccUpgrade.HammerRange);		
			AddVote_Localization(ClientID, "upasive2", "☞ ({int:need}SP) 暴击伤害 +3% ({str:act}) ({int:sum})", "need", &Need, "act", 
				m_apPlayers[ClientID]->AccUpgrade.Pasive2 ? "✔" : "x", "sum", &m_apPlayers[ClientID]->AccUpgrade.Pasive2);			
		}
		else if(m_apPlayers[ClientID]->GetClass() == PLAYERCLASS_BERSERK)
		{
			AddVote_Localization(ClientID, "ushammerrange", "☞ ({int:need}SP) 锤子范围 ({str:act}) ({int:sum})", "need", &Need, "act", 
				m_apPlayers[ClientID]->AccUpgrade.HammerRange ? "✔" : "x", "sum", &m_apPlayers[ClientID]->AccUpgrade.HammerRange);		
			AddVote_Localization(ClientID, "upasive2", "☞ ({int:need}SP) 伤害 +3% ({str:act}) ({int:sum})", "need", &Need, "act", 
				m_apPlayers[ClientID]->AccUpgrade.Pasive2 ? "✔" : "x", "sum", &m_apPlayers[ClientID]->AccUpgrade.Pasive2);			
		}
		AddVote("············", "null", ClientID);
		AddVote_Localization(ClientID, "null", "☭ {str:psevdo}", "psevdo", LocalizeText(ClientID, "Active Skills"));
		AddVote_Localization(ClientID, "uskillwall", "☞ (70SP) Skill Walls ({str:act})", "act", Server()->GetItemCount(ClientID, SKWALL) ? "30 Mana ✔" : "x");	
		SkillSettings(ClientID, SKWALL, "sskillwall");
		AddVote_Localization(ClientID, "uskillheal", "☞ (60SP) Skill Heal ({str:act})", "act", Server()->GetItemCount(ClientID, SKHEAL) ? "50 Mana ✔" : "x");	
		SkillSettings(ClientID, SKHEAL, "sskillheal");
		AddVote_Localization(ClientID, "uskillsword", "☞ (20SP) Skill Sword ({str:act})", "act", Server()->GetItemCount(ClientID, SSWORD) ? "1 Mana ✔" : "x");	
		SkillSettings(ClientID, SSWORD, "sskillsword");
		SkillSettings(ClientID, SHEALSUMMER, "sskillsummer");
		AddBack(ClientID);
		return;
	}
	 
	// ############################### Клан основное меню
	else if(Type == CLAN)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		int ID = Server()->GetClanID(ClientID);
		int Bank = Server()->GetClan(DMONEY, Server()->GetClanID(ClientID));
		int Count = Server()->GetClan(DCOUNTUCLAN, Server()->GetClanID(ClientID));
		int Relevante = Server()->GetClan(DKILL, Server()->GetClanID(ClientID));
		int MaxCount = Server()->GetClan(DMAXCOUNTUCLAN, Server()->GetClanID(ClientID));

		Server()->InitClanID(Server()->GetClanID(ClientID), PLUS, "Init", 0, false);

		AddVote_Localization(ClientID, "null", "公会名称: {str:name}(ID:{int:id})", "name", Server()->ClientClan(ClientID), "id", &ID);
		AddVote_Localization(ClientID, "null", "黄金储量: {int:bank}" , "bank", &Bank);
		AddVote_Localization(ClientID, "null", "会长: {str:leader}", "leader", Server()->LeaderName(Server()->GetClanID(ClientID)));
		AddVote_Localization(ClientID, "null", "关联: {int:revl}", "revl", &Relevante);
		AddVote_Localization(ClientID, "null", "公会人数: {int:count}/{int:maxcount}", "count", &Count , "maxcount", &MaxCount);

		int exp = Server()->GetClan(DEXP, Server()->GetClanID(ClientID));
		int bonus = Server()->GetClan(DADDEXP, Server()->GetClanID(ClientID));
		int level = Server()->GetClan(DLEVEL, Server()->GetClanID(ClientID));
		int dlvl = Server()->GetClan(DADDMONEY, Server()->GetClanID(ClientID))*100;
		int maxneed = Server()->GetClan(DLEVEL, Server()->GetClanID(ClientID))*m_apPlayers[ClientID]->GetNeedForUpClan();
		AddVote_Localization(ClientID, "null", "等级: {int:lvl} 经验 ({int:exp}/{int:maxneed})", "lvl", &level, "exp", &exp , "maxneed", &maxneed);
		AddVote_Localization(ClientID, "null", "奖金: +{int:exp} 经验. +{int:money} 黄金", "exp", &bonus, "money", &dlvl);
		AddVote("", "null", ClientID);
		AddVoteMenu_Localization(ClientID, CMONEY, MENUONLY, "- 捐赠黄金给公会");
		AddVoteMenu_Localization(ClientID, CSHOP, MENUONLY, "- 公会升级");
		AddVoteMenu_Localization(ClientID, CHOUSE, MENUONLY, "- 房屋菜单与升级");

		AddVoteMenu_Localization(ClientID, CLANLIST, MENUONLY, "- 公会列表");
		AddVote_Localization(ClientID, "cexit", "- 退出公会!");
		AddBack(ClientID);
		return;
	}
	
	// ############################### Дом клана
	else if(Type == CHOUSE)
	{ 
		m_apPlayers[ClientID]->m_LastVotelist = CLAN;	
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "数一数二的公会才能拥有房屋");		
		
		if(Server()->GetHouse(ClientID))
		{
			AddVote("", "null", ClientID);
			AddVote_Localization(ClientID, "null", "房屋设置");
			AddVote_Localization(ClientID, "houseopen", "房屋的门 [{str:stat}]", "stat", Server()->GetOpenHouse(Server()->GetOwnHouse(ClientID)) ? "OPEN" : "CLOSE");
			AddVote("", "null", ClientID);
			AddVote_Localization(ClientID, "null", "公会成员的房屋升级");

			int Count = 499999;
			AddVote_Localization(ClientID, "uspawnhouse", "- 购买在房屋内的出生点 [{int:price}]", "price", &Count);
		
			int MaxCount = Server()->GetClan(DCHAIRHOUSE, Server()->GetClanID(ClientID));
			Count = m_apPlayers[ClientID]->GetNeedForUpgClan(DCHAIRHOUSE)*2;
			AddVote_Localization(ClientID, "uchair", "- 升级挂机所获 [{int:max}*(+1) {int:coin}]", "maxcount", &MaxCount, "coin", &Count);

			AddVote("", "null", ClientID);
			AddVote_Localization(ClientID, "null", "房屋里的家具 - 你的房屋-- 1 级");
			AddVote("Comming", "null", ClientID);
		}
		else
		 	AddVote_Localization(ClientID, "null", "你所在的公会还没有房屋呢!");

		AddBack(ClientID);
		return;
	}

	// ############################### Инвент лист
	else if(Type == EVENTLIST)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;	
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "事件列表");
		AddVote("", "null", ClientID);

		bool found = false;
		if(g_Config.m_SvEventSummer)
		{
			found = true;
			AddVote_Localization(ClientID, "null", "事件汇总(summer):");
			AddVote_Localization(ClientID, "null", "杀死怪物时随机获得合成物品");			
			AddVote_Localization(ClientID, "null", "如果成功的话将会随机合成");
			AddVote_Localization(ClientID, "null", "你将得到称号与技能汇总(summer)"); 
			AddVote("", "null", ClientID);
		}
		if(g_Config.m_SvEventHammer)
		{
			found = true;
			AddVote_Localization(ClientID, "null", "事件物品(地上掉的锤子):");
			AddVote_Localization(ClientID, "null", "当杀死怪物时随机获得一种盒子");
			AddVote("", "null", ClientID);
		}
		if(g_Config.m_SvEventSchool)
		{
			found = true;
			AddVote_Localization(ClientID, "null", "事件在线奖励(Back to School):");
			AddVote_Localization(ClientID, "null", "每10分钟你会得到奖励");
			AddVote_Localization(ClientID, "null", "如果你收集了 25 soul patricle");
			AddVote_Localization(ClientID, "null", "你将会得到自定义皮肤道具");
		}
		if(!found)
			AddVote_Localization(ClientID, "null", "肥肠豹潜! 现在没有活动的事件.");

		AddBack(ClientID);
		return;
	}

	// ############################### Лист клана
	else if(Type == CLANLIST)
	{
		m_apPlayers[ClientID]->m_LastVotelist = CLAN;	
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "这是会长处置玩家的菜单");
		AddVote_Localization(ClientID, "null", "For open player settings");
		Server()->ListClan(ClientID, Server()->GetClanID(ClientID));
		AddBack(ClientID);
		AddVote("", "null", ClientID);
		return;
	}
	
	// ############################### Выбор действия над игроком с клана
	else if(Type == CSETTING)
	{
		m_apPlayers[ClientID]->m_LastVotelist = CLANLIST;	
		
		AddVote_Localization(ClientID, "null", "▶ 选择了玩家 {str:name}", "name", m_apPlayers[ClientID]->m_SelectPlayer);
		AddVote_Localization(ClientID, "cgetleader", "▹ 转让公会");
		AddVote_Localization(ClientID, "ckickoff", "▹ 踢出公会");

		AddBack(ClientID);
		return;
	}
	
	// ############################### Магазин клана
	else if(Type == CSHOP)
	{
		m_apPlayers[ClientID]->m_LastVotelist = CLAN;	
		
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "公会成员相关的商店");
		AddVote_Localization(ClientID, "null", "只能由会长来购买");
		AddVote("", "null", ClientID); 
		
		int Count = m_apPlayers[ClientID]->GetNeedForUpgClan(DMAXCOUNTUCLAN)*4;
		int MaxCount = Server()->GetClan(DMAXCOUNTUCLAN, Server()->GetClanID(ClientID));
		AddVote_Localization(ClientID, "null", "公会核心升级");
		AddVote_Localization(ClientID, "uccount", "- 升级公会最大人数 [{int:maxcount}(+1) {int:coin}]", "maxcount", &MaxCount, "coin", &Count);
		
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "升级公会给公会成员的奖金");
		MaxCount = Server()->GetClan(DADDEXP, Server()->GetClanID(ClientID));
		Count = m_apPlayers[ClientID]->GetNeedForUpgClan(DADDEXP);
		AddVote_Localization(ClientID, "uaddexp", "- 升级挂机的经验奖励 [{int:max}*(+1) {int:coin}]", "maxcount", &MaxCount, "coin", &Count);
	
		MaxCount = Server()->GetClan(DADDMONEY, Server()->GetClanID(ClientID));
		Count = m_apPlayers[ClientID]->GetNeedForUpgClan(DADDMONEY);
		AddVote_Localization(ClientID, "uaddmoney", "- 升级挂机的白银奖励 [{int:max}*(+100) {int:coin}]", "maxcount", &MaxCount, "coin", &Count);						
		AddBack(ClientID);
		return;
	}
	
	// ############################### Добавка монет в клан
	else if(Type == CMONEY)
	{
		m_apPlayers[ClientID]->m_LastVotelist = CLAN;	
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "这个菜单可以捐赠黄金给公会的");
		AddVote_Localization(ClientID, "null", "钱是为了大家的");
		AddVote_Localization(ClientID, "null", "在投票的理由填写处填写数量");	
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "cm1", "- 捐赠黄金给公会");
		
		AddBack(ClientID);
		return;
	}
	
	// ############################### Лист разыскиваемых
	else if(Type == RESLIST)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;	
		
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "这是被通缉玩家的列表");
		AddVote_Localization(ClientID, "null", "如果你杀死了列表里的玩家");
		AddVote_Localization(ClientID, "null", "你将会得到回报(白银)，而那个玩家将会进监狱");
		AddVote("", "null", ClientID);
		
		bool Found = false;
		for(int i = 0; i < MAX_NOBOT; ++i)
		{
			if(m_apPlayers[i])
			{
				if(m_apPlayers[i]->m_Search)
				{
					char aBuf[64];
					str_format(aBuf, sizeof(aBuf), "-%s (%d 白银)", Server()->ClientName(i), m_apPlayers[i]->AccData.Level*1000);
					AddVote(aBuf, "null", ClientID);
					
					Found = true;
				}
			}
		}
		if(!Found)
			AddVote_Localization(ClientID, "null", "这里不能搜索(searchable)");
		
		AddBack(ClientID);
		return;
	}
	
	// ############################### Топ меню
	else if(Type == TOPMENU)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;	
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "玩家/公会排名");
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "★ 玩家 - {str:psevdo}", "psevdo", LocalizeText(ClientID, "Players"));
		AddVote_Localization(ClientID, "sort1", "☞ 等级排名");
		AddVote_Localization(ClientID, "sort2", "☞ 黄金排名");
		AddVote_Localization(ClientID, "sort3", "☞ 任务排名");
		AddVote_Localization(ClientID, "sort6", "☞ 击杀排名");
		AddVote_Localization(ClientID, "sort7", "☞ Win Area排名");
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "★ 公会 - {str:psevdo}", "psevdo", LocalizeText(ClientID, "Clans"));		
		AddVote_Localization(ClientID, "sort4", "☞ 等级排名");
		AddVote_Localization(ClientID, "sort5", "☞ 黄金排名");
		AddVote_Localization(ClientID, "sort8", "☞ Relevance排名");

		AddBack(ClientID);
		AddVote("", "null", ClientID);
		
		if(m_apPlayers[ClientID]->m_SortedSelectTop == 1)
			Server()->ShowTop10(ClientID, "Level", 1);
		else if(m_apPlayers[ClientID]->m_SortedSelectTop == 2)
			Server()->ShowTop10(ClientID, "Gold", 1);
		else if(m_apPlayers[ClientID]->m_SortedSelectTop == 3)
			Server()->ShowTop10(ClientID, "Quest", 1);
		else if(m_apPlayers[ClientID]->m_SortedSelectTop == 4)
			Server()->ShowTop10Clans(ClientID, "Level", 1);
		else if(m_apPlayers[ClientID]->m_SortedSelectTop == 5)
			Server()->ShowTop10Clans(ClientID, "Money", 1);
		else if(m_apPlayers[ClientID]->m_SortedSelectTop == 6)
			Server()->ShowTop10(ClientID, "Killing", 1);
		else if(m_apPlayers[ClientID]->m_SortedSelectTop == 7)
			Server()->ShowTop10(ClientID, "WinArea", 1);
		else if(m_apPlayers[ClientID]->m_SortedSelectTop == 8)
			Server()->ShowTop10Clans(ClientID, "Relevance", 1);
			
		return;
	}
	
	// ############################### Инвентарь
	else if(Type == INVENTORY)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "这是你的物品栏");
		AddVote_Localization(ClientID, "null", "选择类型在 () 数量");
		AddVote("", "null", ClientID);

		int Counts = Server()->GetItemCountType(ClientID, 6);
		AddVote_Localization(ClientID, "its6", "☞ 专长与工作 ({int:got})", "got", &Counts);
		Counts = Server()->GetItemCountType(ClientID, 5);
		AddVote_Localization(ClientID, "its5", "☞ 合成与蓝图 ({int:got})", "got", &Counts);
		Counts = Server()->GetItemCountType(ClientID, 4);
		AddVote_Localization(ClientID, "its4", "☞ 消耗品 ({int:got})", "got", &Counts);
		Counts = Server()->GetItemCountType(ClientID, 3);
		AddVote_Localization(ClientID, "its3", "☞ 任务与其他 ({int:got})", "got", &Counts);
		Counts = Server()->GetItemCountType(ClientID, 2);	
		AddVote_Localization(ClientID, "its2", "☞ 稀有物品 ({int:got})", "got", &Counts);
		Counts = Server()->GetItemCountType(ClientID, 1);
		AddVote_Localization(ClientID, "its1", "☞ 武器与升级 ({int:got})", "got", &Counts);
		AddBack(ClientID);
		AddVote("", "null", ClientID);
		if(m_apPlayers[ClientID]->m_SelectItemType == 1)
			Server()->ListInventory(ClientID, 1);
		else if(m_apPlayers[ClientID]->m_SelectItemType == 2)
			Server()->ListInventory(ClientID, 2);	
		else if(m_apPlayers[ClientID]->m_SelectItemType == 3)
			Server()->ListInventory(ClientID, 3);			
		else if(m_apPlayers[ClientID]->m_SelectItemType == 4)
			Server()->ListInventory(ClientID, 4);
		else if(m_apPlayers[ClientID]->m_SelectItemType == 5)
			Server()->ListInventory(ClientID, 5);
		else if(m_apPlayers[ClientID]->m_SelectItemType == 6)
			Server()->ListInventory(ClientID, 6);
		// 1 - Weapon Upgradins, 2 - Rare Artifacts, 3 - Quest Item's, 4 - Useds Items, 5 - Crafted Item, 6 - Work item
		return;
	}
	
	// ############################### Донат меню
	else if(Type == CDONATE)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;	
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "充钱与特权");
		AddVote_Localization(ClientID, "null", "1 欧元 - 100 点券(donate coin)");
		AddVote_Localization(ClientID, "null", "向 Kurosio 捐赠(打钱)");
		AddVote_Localization(ClientID, "null", "或者在 Discord 内进行");
		AddVote_Localization(ClientID, "info", "{str:dis}", "dis", g_Config.m_Discord);
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "null", "$ 你充了 {int:don}", "don", &m_apPlayers[ClientID]->AccData.Donate);
		AddVote_Localization(ClientID, "bvip", "☞ VIP 包 [1000]");
		AddVote_Localization(ClientID, "null", "物品 禁止PVP, 技能点(SP)盒子, 10,000 钱袋");
		AddVote_Localization(ClientID, "null", "X2 - 若干钱与经验 + 特殊物品 Snap Draw");
		AddVote_Localization(ClientID, "bsp", "☞ 技能点盒子(SP) [200]");
		AddVote_Localization(ClientID, "null", "获得 20 升级点 + 10 技能点");
		AddVote_Localization(ClientID, "bantipvp", "☞ 物品 禁止PVP [200]");
		AddVote_Localization(ClientID, "null", "你将会获得禁止PVP设置");
		AddVote("", "null", ClientID);
		AddBack(ClientID);
		return;
	}
	

	// ############################### Инвентарь действие -- 选择物品后的操作
	else if(Type == SELITEM)
	{
		int SelectItem = m_apPlayers[ClientID]->m_SelectItem;
		if(Server()->GetItemType(ClientID, SelectItem) == 15 || Server()->GetItemType(ClientID, SelectItem) == 16 
			|| Server()->GetItemType(ClientID, SelectItem) == 17)
			m_apPlayers[ClientID]->m_LastVotelist = ARMORMENU;
		else m_apPlayers[ClientID]->m_LastVotelist = INVENTORY;	

		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", Server()->GetItemDesc(ClientID, SelectItem));	
		AddVote("", "null", ClientID);	
		AddVote_Localization(ClientID, "null", "在投票的理由填写处填写数量");	
		AddVote_Localization(ClientID, "null", "如果投票的理由处不是一个数字或没有东西，就使用一个");	
		AddVote("", "null", ClientID);	
		AddVote_Localization(ClientID, "null", "▶ 你选中了物品 {str:name}", "name", Server()->GetItemName(ClientID, m_apPlayers[ClientID]->m_SelectItem));
		
		if(Server()->GetItemType(ClientID, SelectItem) == 4)
			AddVote_Localization(ClientID, "useitem", "▹ 使用该物品");
			
		if(Server()->GetItemPrice(ClientID, SelectItem, 1) > 0 && m_apPlayers[ClientID]->GetWork())
			AddVote_Localization(ClientID, "sellitem", "▹ 卖出该物品");

		if(Server()->GetItemType(ClientID, SelectItem) != 6)
			AddVote_Localization(ClientID, "dropitem", "▹ 丢掉该物品 (╯°Д°)╯");
		
		if(Server()->GetItemType(ClientID, SelectItem) == 15 || Server()->GetItemType(ClientID, SelectItem) == 16
			|| Server()->GetItemType(ClientID, SelectItem) == 17)
		{
			AddVote("", "null", ClientID);	
			AddVote_Localization(ClientID, "enchantitem", "▹ 附魔该物品 (需要 1000 个材料[material])");	
			AddVote("", "null", ClientID);	
		}
		AddVote_Localization(ClientID, "destitem", "▹ 摧毁物品 ㄟ( ▔, ▔ )ㄏ ");
		AddBack(ClientID);
		return;
	}
	
	// ############################### Квесты меню -- 任务相关
	else if(Type == QUEST)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;	
		if(m_apPlayers[ClientID]->GetCharacter() && m_apPlayers[ClientID]->GetCharacter()->InQuest())
		{	
			bool Trying = true;
			AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
			AddVote_Localization(ClientID, "null", "任务菜单");
			AddVote("", "null", ClientID);
			if(m_apPlayers[ClientID]->AccData.Quest == 1)
			{
				int Need = QUEST1, Counts = Server()->GetItemCount(ClientID, PIGPORNO);
				AddVote_Localization(ClientID, "null", "可爱的佩奇 I");
				AddVote_Localization(ClientID, "null", "拿起你的杀猪刀朝可爱的小猪砍去，");
				AddVote_Localization(ClientID, "null", "你将有机会会获得它的肉。");
				AddVote_Localization(ClientID, "null", "[已获得: {int:get}/共需要: {int:need}]", "get", &Counts, "need", &Need);
				AddVote_Localization(ClientID, "null", "任务奖励: {str:got}", "got", "4000经验/200000白银");
			}
			else if(m_apPlayers[ClientID]->AccData.Quest == 2)
			{
				int Need = QUEST2, Counts = Server()->GetItemCount(ClientID, PIGPORNO);
				AddVote_Localization(ClientID, "null", "可爱的佩奇 II");
				AddVote_Localization(ClientID, "null", "拿起你的杀猪刀朝可爱的小猪砍去，");
				AddVote_Localization(ClientID, "null", "你将有机会会获得它的肉。");
				AddVote_Localization(ClientID, "null", "[已获得: {int:get}/共需要: {int:need}]", "get", &Counts, "need", &Need);
				AddVote_Localization(ClientID, "null", "任务奖励: {str:got}", "got", "4000经验/250000白银");
			}
			else if(m_apPlayers[ClientID]->AccData.Quest == 3)
			{
				int Need = QUEST3, Counts = Server()->GetItemCount(ClientID, KWAHGANDON);
				AddVote_Localization(ClientID, "null", "Kwah I");
				AddVote_Localization(ClientID, "null", "杀死盘踞在矿坑中部的kwah们,");
				AddVote_Localization(ClientID, "null", "把它们的头给我带过来!");
				AddVote_Localization(ClientID, "null", "[已获得: {int:get}/共需要: {int:need}]", "get", &Counts, "need", &Need);
				AddVote_Localization(ClientID, "null", "任务奖励: {str:got}", "got", "8000经验/500000白银");
			}
			else if(m_apPlayers[ClientID]->AccData.Quest == 4)
			{
				int Need = QUEST4, Counts = Server()->GetItemCount(ClientID, KWAHGANDON);
				AddVote_Localization(ClientID, "null", "Kwah II");
				AddVote_Localization(ClientID, "null", "杀死盘踞在矿坑中部的kwah们,");
				AddVote_Localization(ClientID, "null", "把它们的头给我带过来!");
				AddVote_Localization(ClientID, "null", "[已获得: {int:get}/共需要: {int:need}]", "get", &Counts, "need", &Need);
				AddVote_Localization(ClientID, "null", "任务奖励: {str:got}", "got", "8000经验/550000白银");
			}	
			else if(m_apPlayers[ClientID]->AccData.Quest == 5)
			{
				int Need = QUEST5; 
				int Counts = Server()->GetItemCount(ClientID, PIGPORNO);
				int Counts2 = Server()->GetItemCount(ClientID, KWAHGANDON);
				AddVote_Localization(ClientID, "null", "佩奇与Kwah [第一步] - 猪肉, Kwah 头 [{int:get}/{int:need} & {int:get2}/{int:need2}]", "get", &Counts, "need", &Need, "get2", &Counts2, "need2", &Need);
				AddVote_Localization(ClientID, "null", "你一共有 {str:got} 个所需物品", "got", "Kwah 耳环, 1000000白银");
			}
			else if(m_apPlayers[ClientID]->AccData.Quest == 6)
			{
				int Need = QUEST6;
				int Counts = Server()->GetItemCount(ClientID, KWAHGANDON);
				int Counts2 = Server()->GetItemCount(ClientID, FOOTKWAH);
				AddVote_Localization(ClientID, "null", "Kwah [第一步] - Kwah 头, Kwah 脚 [{int:get}/{int:need} & {int:get2}/{int:need2}]", "get", &Counts, "need", &Need, "get2", &Counts2, "need2", &Need);
				AddVote_Localization(ClientID, "null", "你一共有 {str:got} 个所需物品", "got", "武器的蓝图, 1050000白银");
				AddVote_Localization(ClientID, "null", "+ 称号 ♥任务_#");
			}
			else
			{
				Trying = false;
				AddVote_Localization(ClientID, "null", "任务不可用");
			}
			if(Trying)
				AddVote_Localization(ClientID, "passquest", "- 提交通过任务");
		}
		else AddVote_Localization(ClientID, "null", "你不在任务大厅(Quest Room)");
			
		AddBack(ClientID);
		return;
	}
	
	// ############################### Крафтинг меню
	else if(Type == CRAFTING)
	{
		m_apPlayers[ClientID]->m_LastVotelist = AUTH;	
		AddVote_Localization(ClientID, "null", "☪ 信息 ( ′ ω ` )?:");
		AddVote_Localization(ClientID, "null", "合成菜单");
		AddVote("", "null", ClientID);
		AddVote_Localization(ClientID, "scr1", "▹ 基础物品");
		AddVote_Localization(ClientID, "scr2", "▹ 神器(Artifacts)");
		AddVote_Localization(ClientID, "scr3", "▹ 配件(Modules)与武器");
		AddVote_Localization(ClientID, "scr4", "▹ 增益与食物");
		AddVote_Localization(ClientID, "scr5", "▹ 工作专长相关");
		AddVote_Localization(ClientID, "scr6", "▹ 装备");

		AddBack(ClientID);
		AddVote("", "null", ClientID);
		
		if(m_apPlayers[ClientID]->GetCharacter() && m_apPlayers[ClientID]->GetCharacter()->InCrafted())
		{
			if(m_apPlayers[ClientID]->m_SortedSelectCraft == 1)
			{
				AddNewCraftVote(ClientID, "骷髅骨头x30", SKELETSSBONE);	
				AddNewCraftVote(ClientID, "僵尸眼x30", ZOMIBEBIGEYE);	
				AddNewCraftVote(ClientID, "(铁矿, 铜矿)x100", FORMULAEARRINGS);	
				AddNewCraftVote(ClientID, "(铁矿, 铜矿)x125", FORMULAFORRING);	
				AddNewCraftVote(ClientID, "(铁矿, 铜矿)x150", FORMULAWEAPON);	
					
			}
			else if(m_apPlayers[ClientID]->m_SortedSelectCraft == 2)
			{
				if(g_Config.m_SvEventSummer)
					AddNewCraftVote(ClientID, "日耀(Sun Ray)x20", SHEALSUMMER);	

				AddNewCraftVote(ClientID, "戒指的蓝图, Slime Dirt", RARERINGSLIME);	
				AddNewCraftVote(ClientID, "戒指的蓝图, 爆破鬼才的尸体 x100", RINGBOOMER);	
				AddNewCraftVote(ClientID, "耳环的蓝图, Kwah 脚 x 100", EARRINGSKWAH);	
				AddNewCraftVote(ClientID, "(僵尸大眼睛与骷髅强化骨)x30", CUSTOMSKIN);	
				AddNewCraftVote(ClientID, "(土豆番茄萝卜)x60", JUMPIMPULS);		
			}
			else if(m_apPlayers[ClientID]->m_SortedSelectCraft == 3)
			{
				AddNewCraftVote(ClientID, "眼睛表情 (happy, evil, surprise, blink, pain)", MODULEEMOTE);	
				AddNewCraftVote(ClientID, "武器 (手枪, 散弹枪, 榴弹炮, 激光枪)", WEAPONPRESSED);	
				AddNewCraftVote(ClientID, "武器蓝图, 爆破鬼才的戒指", MODULESHOTGUNSLIME);	
				AddNewCraftVote(ClientID, "武器蓝图 x25", ENDEXPLOSION);	
			}
			else if(m_apPlayers[ClientID]->m_SortedSelectCraft == 4)
			{
				
			}
			else if(m_apPlayers[ClientID]->m_SortedSelectCraft == 5)
			{
				AddNewCraftVote(ClientID, "木头x30, 铜矿x60", COOPERPIX);		
				AddNewCraftVote(ClientID, "木头x40, 铁矿x60", IRONPIX);		
				AddNewCraftVote(ClientID, "木头x50, 金矿x80", GOLDPIX);		
				AddNewCraftVote(ClientID, "木头x50, 钻石矿x100", DIAMONDPIX);					
			}
			else if(m_apPlayers[ClientID]->m_SortedSelectCraft == 6)
			{
				AddNewCraftVote(ClientID, "皮革x50, 木头x150", LEATHERBODY);		
				AddNewCraftVote(ClientID, "皮革x40, 木头x120", LEATHERFEET);
				AddNewCraftVote(ClientID, "铜矿x500, 木头x150", COOPERBODY);		
				AddNewCraftVote(ClientID, "铜矿x400, 木头x120", COOPERFEET);
				AddNewCraftVote(ClientID, "铁矿x500, 木头x150", IRONBODY);		
				AddNewCraftVote(ClientID, "铁矿x400, 木头x120", IRONFEET);
				AddNewCraftVote(ClientID, "金矿x500, 木头x150", GOLDBODY);		
				AddNewCraftVote(ClientID, "金矿x400, 木头x120", GOLDFEET);
				AddNewCraftVote(ClientID, "钻石矿x500, 木头x150", DIAMONDBODY);		
				AddNewCraftVote(ClientID, "钻石矿x400, 木头x120", DIAMONDFEET);
				AddNewCraftVote(ClientID, "龙矿x500, 木头x150", DRAGONBODY);		
				AddNewCraftVote(ClientID, "龙矿x400, 木头x120", DRAGONFEET);
				AddNewCraftVote(ClientID, "铜矿x100, 铁矿x10", STCLASIC);						
			}
		}
		else AddVote_Localization(ClientID, "null", "你不在合成室(Craft Room)");

		return;
	}
}

void CGameContext::AddNewCraftVote(int ClientID, const char *Need, int ItemID)
{
	AddVote_Localization(ClientID, "null", "物品: {str:name}", "name", Server()->GetItemName(ClientID, ItemID));
	AddVote_Localization(ClientID, "null", "需要: {str:need}", "need", Need);
	AddVote_Localization(ClientID, "null", "Desc {str:desc}", "desc", Server()->GetItemDesc(ClientID, ItemID));
	AddVoteMenu_Localization(ClientID, ItemID, CRAFTONLY, "- 合成 {str:name} !", "name", Server()->GetItemName(ClientID, ItemID));
	AddVote("--------------------", "null", ClientID);		
	return;	
}

void CGameContext::AddBack(int ClientID)
{	
	AddVote("", "null", ClientID);
	AddVote_Localization(ClientID, "back", "- 返回上一级菜单");
}

void CGameContext::CreateSellWorkItem(int ClientID, int ItemID, int Price)
{
	Server()->SetItemPrice(ClientID, ItemID, 1, Price*2);
	int Count = Server()->GetItemCount(ClientID, ItemID);
	AddVoteMenu_Localization(ClientID, ItemID, SELLITEMWORK, "出售 {str:name}:{int:count} [每一个获得{int:price}黄金]", "name", Server()->GetItemName(ClientID, ItemID), "count", &Count, "price", &Price);
	return;
}

void CGameContext::CreateNewShop(int ClientID, int ItemID, int Type, int Level, int Price)
{
	switch(Type)
	{
		case 2:
		{	
			Server()->SetItemPrice(ClientID, ItemID, Level, Price);
			int PriceNow = Server()->GetItemPrice(ClientID, ItemID, 1);
			
			int Count = Server()->GetItemCount(ClientID, ItemID);	
			AddVoteMenu_Localization(ClientID, ItemID, BUYITEMONLY, "({int:al}) (Lv:{int:need}) {str:itemname} [{int:price}黄金]", "al", &Count, "need", &Level, "itemname", Server()->GetItemName(ClientID, ItemID), "price", &PriceNow);
		} break;
		case 3:
		{
			Server()->SetItemPrice(ClientID, ItemID, Level, Price);
			int PriceNow = Server()->GetItemPrice(ClientID, ItemID, 1);

			const char* laserd = Server()->GetItemCount(ClientID, ItemID) ? "✔" : "x";				
			AddVoteMenu_Localization(ClientID, ItemID, BUYITEMONLY, "({str:al}) (Lv:{int:need}) {str:itemname} [{int:price}黄金]", "al", laserd, "need", &Level, "itemname", Server()->GetItemName(ClientID, ItemID), "price", &PriceNow);	
		} break;
		default:
		{
			if(Server()->GetItemCount(ClientID, ItemID))
			{
				const char* Data = Server()->GetItemSettings(ClientID, ItemID) ? "☑" : "☐";
				AddVoteMenu_Localization(ClientID, ItemID, SETTINGSONLY, "☞ {str:stat} {str:itemname}", "stat", Data, "itemname", Server()->GetItemName(ClientID, ItemID));
				if(Server()->GetItemType(ClientID, ItemID) == 10)
					AddVoteMenu_Localization(ClientID, ItemID, SETTINGSONLY, "{str:name}", "name", Server()->GetItemDesc(ClientID, ItemID));
			}
		} break;
	}
	return;
}

int CGameContext::GetAreaCount()
{
	int Count = 0;
	for(int i = 0; i < MAX_NOBOT; ++i)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->m_InArea)
			Count++;
	}
	return Count;
}

void CGameContext::StartArea(int WaitTime, int Type)
{		
	m_AreaStartTick = Server()->TickSpeed()*WaitTime;
	m_AreaType = Type;

	const char* NameGame = "NONE";
	int Gets = 0;
	switch(m_AreaType)
	{
		case 1: NameGame = "Insta"; Gets = 50; break;
		case 2: NameGame = "FNG"; Gets = 5; break;
	}
	SendChatTarget(-1, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
	SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("[Survial] Registration 开始了. {str:name}"), "name", NameGame, NULL);
	SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("[Survial] 回报: 钱袋, 神器 {int:gets}%"), "gets", &Gets, NULL);
	SendChatTarget(-1, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
}

void CGameContext::EnterArea(int ClientID)
{	
	if(!m_apPlayers[ClientID] || !m_apPlayers[ClientID]->GetCharacter())
		return;
		
	if(m_apPlayers[ClientID]->m_JailTick || m_apPlayers[ClientID]->m_Search)
		return 	SendBroadcast_Localization(ClientID, 250, 150, _("你被通缉了. 不能进入area room."));
		
	if(!m_AreaStartTick)
		return 	SendBroadcast_Localization(ClientID, 250, 150, _("Survial not registration wait registration"));
			
	m_apPlayers[ClientID]->m_InArea = true;
	m_apPlayers[ClientID]->GetCharacter()->Die(ClientID, WEAPON_WORLD);
}

void CGameContext::AddVote(const char *Desc, const char *Cmd, int ClientID)
{
	while(*Desc && *Desc == ' ')
		Desc++;

	if(ClientID == -2)
		return;

	CVoteOptions Vote;	
	str_copy(Vote.m_aDescription, Desc, sizeof(Vote.m_aDescription));
	str_copy(Vote.m_aCommand, Cmd, sizeof(Vote.m_aCommand));
	m_PlayerVotes[ClientID].add(Vote);
	
	// inform clients about added option
	CNetMsg_Sv_VoteOptionAdd OptionMsg;	
	OptionMsg.m_pDescription = Vote.m_aDescription;
	Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, ClientID);
}

bool CGameContext::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CNetMsg_Sv_Motd Msg;
		Msg.m_pMessage = g_Config.m_SvMotd;
		CGameContext *pSelf = (CGameContext *)pUserData;
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(pSelf->m_apPlayers[i])
				pSelf->Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
	}
	return true;
}

bool CGameContext::PrivateMessage(const char* pStr, int ClientID, bool TeamChat)
{	
	bool ArgumentFound = false;
	const char* pArgumentIter = pStr;
	while(*pArgumentIter)
	{
		if(*pArgumentIter != ' ')
		{
			ArgumentFound = true;
			break;
		}
		pArgumentIter++;
	}
	
	if(!ArgumentFound)
	{
		SendChatTarget(ClientID, "用法: /msg <用户名或群体> <信息>");
		SendChatTarget(ClientID, "向玩家或一群玩家发送私聊消息");
		SendChatTarget(ClientID, "可用的群体名: #clan(公会名)");
		return true;
	}
	
	dynamic_string FinalMessage;
	int TextIter = 0;
	
	int CheckID = -1;
	int CheckClan = -1;
	
	char aNameFound[32];
	aNameFound[0] = 0;
	
	char aChatTitle[32];
	aChatTitle[0] = 0;
	unsigned int c = 0;
	for(; c<sizeof(aNameFound)-1; c++)
	{
		if(pStr[c] == ' ' || pStr[c] == 0)
		{
			if(str_comp(aNameFound, "#clan") == 0 && m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetCharacter())
			{
				if(Server()->GetClanID(ClientID))
				{
					CheckClan = Server()->GetClanID(ClientID);
					str_copy(aChatTitle, "clan", sizeof(aChatTitle));
				}
			}
			else
			{
				for(int i=0; i<MAX_NOBOT; i++)
				{
					if(m_apPlayers[i] && str_comp(Server()->ClientName(i), aNameFound) == 0)
					{
						CheckID = i;
						str_copy(aChatTitle, "private", sizeof(aChatTitle));
						break;
					}
				}
			}
		}
		
		if(aChatTitle[0] || pStr[c] == 0)
		{
			aNameFound[c] = 0;
			break;
		}
		else
		{
			aNameFound[c] = pStr[c];
			aNameFound[c+1] = 0;
		}
	}	
	if(!aChatTitle[0])
	{
		SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("没有找到该名字的玩家"));
		return true;
	}
	
	pStr += c;
	while(*pStr == ' ')
		pStr++;
	
	dynamic_string Buffer;
	Buffer.copy(pStr);
	Server()->Localization()->ArabicShaping(Buffer);
	
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = (TeamChat ? 1 : 0);
	Msg.m_ClientID = ClientID;
	
	int NumPlayerFound = 0;
	for(int i=0; i<MAX_NOBOT; i++)
	{
		if(m_apPlayers[i])
		{
			if(i != ClientID)
			{
				if(CheckID >= 0 && !(i == CheckID))
					continue;
				
				if(CheckClan >= 0 && !(Server()->GetClanID(i) == CheckClan))
					continue;
			}
			
			FinalMessage.clear();
			TextIter = 0;
			if(i == ClientID)
			{
				if(str_comp(aChatTitle, "private") == 0)
				{
					TextIter = FinalMessage.append_at(TextIter, aNameFound);
					TextIter = FinalMessage.append_at(TextIter, " (");
					TextIter = FinalMessage.append_at(TextIter, aChatTitle);
					TextIter = FinalMessage.append_at(TextIter, "): ");
				}
				else
				{
					TextIter = FinalMessage.append_at(TextIter, "(");
					TextIter = FinalMessage.append_at(TextIter, aChatTitle);
					TextIter = FinalMessage.append_at(TextIter, "): ");
				}
				TextIter = FinalMessage.append_at(TextIter, Buffer.buffer());
			}
			else
			{
				TextIter = FinalMessage.append_at(TextIter, Server()->ClientName(i));
				TextIter = FinalMessage.append_at(TextIter, " (");
				TextIter = FinalMessage.append_at(TextIter, aChatTitle);
				TextIter = FinalMessage.append_at(TextIter, "): ");
				TextIter = FinalMessage.append_at(TextIter, Buffer.buffer());
			}
			Msg.m_pMessage = FinalMessage.buffer();
	
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
				
			NumPlayerFound++;
		}
	}
	Buffer.clear();
	return true;
}

void CGameContext::ExitClan(int ClientID)
{
	if(Server()->GetClanID(ClientID))
	{
		SendChatClan(Server()->GetClanID(ClientID), _("玩家 {str:name} 退出了公会"), "name", Server()->ClientName(ClientID), NULL);
		m_apPlayers[ClientID]->m_LoginSync = 150;
		Server()->ExitClanOff(ClientID, Server()->ClientName(ClientID));
	}
	return;
}

void CGameContext::EnterClan(int ClientID, int ClanID)
{
	if(!Server()->GetClanID(ClientID))
	{
		Server()->EnterClan(ClientID, ClanID);
		m_apPlayers[ClientID]->m_LoginSync = 150;
		SendChatClan(Server()->GetClanID(ClientID), _("玩家 {str:name} 加入了公会"), "name", Server()->ClientName(ClientID), NULL);
	}
	return;
}

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();

	Console()->Register("tune", "s<param> i<value>", CFGFLAG_SERVER, ConTuneParam, this, "Tune variable to value");
	Console()->Register("tune_reset", "", CFGFLAG_SERVER, ConTuneReset, this, "Reset tuning");
	Console()->Register("tune_dump", "", CFGFLAG_SERVER, ConTuneDump, this, "Dump tuning");

	Console()->Register("pause", "", CFGFLAG_SERVER, ConPause, this, "Pause/unpause game");
	Console()->Register("restart", "?i<sec>", CFGFLAG_SERVER|CFGFLAG_STORE, ConRestart, this, "Restart in x seconds (0 = abort)");
	Console()->Register("broadcast", "r<message>", CFGFLAG_SERVER, ConBroadcast, this, "Broadcast message");
	Console()->Register("say", "r", CFGFLAG_SERVER, ConSay, this, "Say in chat");
	Console()->Chain("sv_motd", ConchainSpecialMotdupdate, this);
}

void CGameContext::OnInit(/*class IKernel *pKernel*/)
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_World.SetGameServer(this);
	m_Events.SetGameServer(this);

	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	m_Layers.Init(Kernel());
	m_Collision.Init(&m_Layers);
	
	//Get zones
	m_ZoneHandle_Damage = m_Collision.GetZoneHandle("icDamage");
	m_ZoneHandle_Teleport = m_Collision.GetZoneHandle("icTele");
	m_ZoneHandle_Bonus = m_Collision.GetZoneHandle("icBonus");

	// select gametype
	m_pController = new CGameControllerMOD(this);

	// create all entities from entity layers
	if(m_Layers.EntityGroup())
	{
		char aLayerName[12];
		
		const CMapItemGroup* pGroup = m_Layers.EntityGroup();
		for(int l = 0; l < pGroup->m_NumLayers; l++)
		{
			CMapItemLayer *pLayer = m_Layers.GetLayer(pGroup->m_StartLayer+l);
			if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				CMapItemLayerQuads *pQLayer = (CMapItemLayerQuads *)pLayer;
				IntsToStr(pQLayer->m_aName, sizeof(aLayerName)/sizeof(int), aLayerName);
				const CQuad *pQuads = (const CQuad *) Kernel()->RequestInterface<IMap>()->GetDataSwapped(pQLayer->m_Data);

				for(int q = 0; q < pQLayer->m_NumQuads; q++)
				{
					vec2 P0(fx2f(pQuads[q].m_aPoints[0].x), fx2f(pQuads[q].m_aPoints[0].y));
					vec2 P1(fx2f(pQuads[q].m_aPoints[1].x), fx2f(pQuads[q].m_aPoints[1].y));
					vec2 P2(fx2f(pQuads[q].m_aPoints[2].x), fx2f(pQuads[q].m_aPoints[2].y));
					vec2 P3(fx2f(pQuads[q].m_aPoints[3].x), fx2f(pQuads[q].m_aPoints[3].y));
					vec2 Pivot(fx2f(pQuads[q].m_aPoints[4].x), fx2f(pQuads[q].m_aPoints[4].y));
					m_pController->OnEntity(aLayerName, Pivot, P0, P1, P2, P3, pQuads[q].m_PosEnv);
				}
			}
		}
	}

	int CurID = 0;
	if(!g_Config.m_SvCityStart)
	{
		for (int o=0; o<12; o++,CurID++)
			CreateBot(CurID, BOT_L1MONSTER, g_Config.m_SvCityStart);
		for (int o=0; o<11; o++,CurID++)
			CreateBot(CurID, BOT_L2MONSTER, g_Config.m_SvCityStart);
		for (int o=0; o<10; o++,CurID++)
			CreateBot(CurID, BOT_L3MONSTER, g_Config.m_SvCityStart);
		for (int o=0; o<1; o++, CurID++)
			CreateBot(CurID, BOT_FARMER, o);
	}
	else if(g_Config.m_SvCityStart == 1)
	{
		for (int o=0; o<11; o++,CurID++)
			CreateBot(CurID, BOT_L1MONSTER, g_Config.m_SvCityStart);
		for (int o=0; o<11; o++,CurID++)
			CreateBot(CurID, BOT_L2MONSTER, g_Config.m_SvCityStart);
		for (int o=0; o<12; o++,CurID++)
			CreateBot(CurID, BOT_L3MONSTER, g_Config.m_SvCityStart);
	}
	for (int o=0; o<1; o++,CurID++)
		CreateBot(CurID, BOT_NPC, g_Config.m_SvCityStart);
	for (int o=0; o<3; o++, CurID++)
		CreateBot(CurID, BOT_NPCW, o);


#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies ; i++)
		{
			OnClientConnected(MAX_CLIENTS-i-1);
		}
	}
#endif

	Server()->InitInvID();
	Server()->InitClan();
	Server()->GetTopClanHouse();
	Server()->InitMaterialID();
}

void CGameContext::OnShutdown()
{
	for (int i=0; i<g_Config.m_SvMaxClients; i++)
	{
		if (!m_apPlayers[i])
			continue;

		delete m_apPlayers[i];
		m_apPlayers[i] = 0x0;
	}

	delete m_pController;
	m_pController = 0;
	Clear();
}

void CGameContext::OnSnap(int ClientID)
{
	m_World.Snap(ClientID);
	m_pController->Snap(ClientID);
	m_Events.Snap(ClientID);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
			m_apPlayers[i]->Snap(ClientID);
	}
}

// ---------------------------- СТАТЫ
void CGameContext::GetStat(int ClientID) //set stat mysql pdata
{
	m_apPlayers[ClientID]->AccData.Level =  Server()->GetStat(ClientID, DLEVEL);
	m_apPlayers[ClientID]->AccData.Exp =  Server()->GetStat(ClientID, DEXP);
	m_apPlayers[ClientID]->AccData.Money =  Server()->GetStat(ClientID, DMONEY);
	m_apPlayers[ClientID]->AccData.Gold =  Server()->GetStat(ClientID, DGOLD);
	m_apPlayers[ClientID]->AccData.Donate =  Server()->GetStat(ClientID, DDONATE);
	m_apPlayers[ClientID]->AccData.Rel =  Server()->GetStat(ClientID, DREL);
	m_apPlayers[ClientID]->AccData.Jail =  Server()->GetStat(ClientID, DJAIL);
	m_apPlayers[ClientID]->AccData.Class =  Server()->GetStat(ClientID, DCLASS);
	m_apPlayers[ClientID]->AccData.Quest =  Server()->GetStat(ClientID, DQUEST);
	m_apPlayers[ClientID]->AccData.Kill =  Server()->GetStat(ClientID, DKILL);
	m_apPlayers[ClientID]->AccData.WinArea =  Server()->GetStat(ClientID, DWINAREA);
	m_apPlayers[ClientID]->AccData.ClanAdded =  Server()->GetStat(ClientID, DCLANADDED);
	return;
}
void CGameContext::UpdateStat(int ClientID) //update stat mysql pdata
{
	Server()->UpdateStat(ClientID, DLEVEL, m_apPlayers[ClientID]->AccData.Level);
	Server()->UpdateStat(ClientID, DEXP, m_apPlayers[ClientID]->AccData.Exp);
	Server()->UpdateStat(ClientID, DMONEY, m_apPlayers[ClientID]->AccData.Money);
	Server()->UpdateStat(ClientID, DGOLD, m_apPlayers[ClientID]->AccData.Gold);
	Server()->UpdateStat(ClientID, DDONATE, m_apPlayers[ClientID]->AccData.Donate);
	Server()->UpdateStat(ClientID, DREL, m_apPlayers[ClientID]->AccData.Rel);
	Server()->UpdateStat(ClientID, DJAIL, m_apPlayers[ClientID]->AccData.Jail);
	Server()->UpdateStat(ClientID, DCLASS, m_apPlayers[ClientID]->AccData.Class);
	Server()->UpdateStat(ClientID, DQUEST, m_apPlayers[ClientID]->AccData.Quest);
	Server()->UpdateStat(ClientID, DKILL, m_apPlayers[ClientID]->AccData.Kill);
	Server()->UpdateStat(ClientID, DWINAREA, m_apPlayers[ClientID]->AccData.WinArea);
	Server()->UpdateStat(ClientID, DCLANADDED, m_apPlayers[ClientID]->AccData.ClanAdded);
	return;
}
void CGameContext::UpdateStats(int ClientID)
{
	UpdateStat(ClientID);
	Server()->UpdateStats(ClientID);	
}

void CGameContext::GetUpgrade(int ClientID) //set stat mysql pdata
{
	m_apPlayers[ClientID]->AccUpgrade.Upgrade = Server()->GetUpgrade(ClientID, SUPGRADE);
	m_apPlayers[ClientID]->AccUpgrade.SkillPoint = Server()->GetUpgrade(ClientID, SKILLPOINT);
	m_apPlayers[ClientID]->AccUpgrade.Speed = Server()->GetUpgrade(ClientID, ASPEED);
	m_apPlayers[ClientID]->AccUpgrade.Damage = Server()->GetUpgrade(ClientID, BDAMAGE);
	m_apPlayers[ClientID]->AccUpgrade.Health = Server()->GetUpgrade(ClientID, HHEALTH);
	m_apPlayers[ClientID]->AccUpgrade.HPRegen = Server()->GetUpgrade(ClientID, HPREGEN);
	m_apPlayers[ClientID]->AccUpgrade.AmmoRegen = Server()->GetUpgrade(ClientID, AMMOREGEN);
	m_apPlayers[ClientID]->AccUpgrade.Ammo = Server()->GetUpgrade(ClientID, AMMO);
	m_apPlayers[ClientID]->AccUpgrade.Spray = Server()->GetUpgrade(ClientID, SPRAY);
	m_apPlayers[ClientID]->AccUpgrade.Mana = Server()->GetUpgrade(ClientID, MANA);
	m_apPlayers[ClientID]->AccUpgrade.HammerRange = Server()->GetUpgrade(ClientID, UHAMMERRANGE);
	m_apPlayers[ClientID]->AccUpgrade.Pasive2 = Server()->GetUpgrade(ClientID, PASIVE2);
	return;
}
void CGameContext::UpdateUpgrade(int ClientID) //update stat mysql pdata
{
	Server()->UpdateUpgrade(ClientID, SUPGRADE, m_apPlayers[ClientID]->AccUpgrade.Upgrade);
	Server()->UpdateUpgrade(ClientID, SKILLPOINT, m_apPlayers[ClientID]->AccUpgrade.SkillPoint);
	Server()->UpdateUpgrade(ClientID, ASPEED, m_apPlayers[ClientID]->AccUpgrade.Speed);
	Server()->UpdateUpgrade(ClientID, BDAMAGE, m_apPlayers[ClientID]->AccUpgrade.Damage);
	Server()->UpdateUpgrade(ClientID, HHEALTH, m_apPlayers[ClientID]->AccUpgrade.Health);
	Server()->UpdateUpgrade(ClientID, HPREGEN, m_apPlayers[ClientID]->AccUpgrade.HPRegen);
	Server()->UpdateUpgrade(ClientID, AMMOREGEN, m_apPlayers[ClientID]->AccUpgrade.AmmoRegen);
	Server()->UpdateUpgrade(ClientID, AMMO, m_apPlayers[ClientID]->AccUpgrade.Ammo);
	Server()->UpdateUpgrade(ClientID, SPRAY, m_apPlayers[ClientID]->AccUpgrade.Spray);
	Server()->UpdateUpgrade(ClientID, MANA, m_apPlayers[ClientID]->AccUpgrade.Mana);
	Server()->UpdateUpgrade(ClientID, UHAMMERRANGE, m_apPlayers[ClientID]->AccUpgrade.HammerRange);
	Server()->UpdateUpgrade(ClientID, PASIVE2, m_apPlayers[ClientID]->AccUpgrade.Pasive2);
	return;
}
void CGameContext::UpdateUpgrades(int ClientID)
{
	UpdateUpgrade(ClientID);
	Server()->UpdateStats(ClientID, 1);	
}

void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	m_Events.Clear();
}

bool CGameContext::IsClientReady(int ClientID)
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->m_IsReady ? true : false;
}

bool CGameContext::IsClientPlayer(int ClientID)
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS ? false : true;
}

const char *CGameContext::GameType() { return m_pController && m_pController->m_pGameType ? m_pController->m_pGameType : ""; }
const char *CGameContext::Version() { return GAME_VERSION; }
const char *CGameContext::NetVersion() { return GAME_NETVERSION; }

IGameServer *CreateGameServer() { return new CGameContext; }

void CGameContext::UpdateBotInfo(int ClientID)
{
	char NameSkin[32];
	const int BotType = m_apPlayers[ClientID]->GetBotType();
	const int BotSubType = m_apPlayers[ClientID]->GetBotSubType();

	if(BotType == BOT_L1MONSTER)
	{
		if(!BotSubType)	str_copy(NameSkin, "pinky", sizeof(NameSkin));
		else if(BotSubType == 1)	str_copy(NameSkin, "twintri", sizeof(NameSkin));
	}
	else if(BotType == BOT_L2MONSTER)
	{
		if(!BotSubType)	str_copy(NameSkin, "cammostripes", sizeof(NameSkin));
		else if(BotSubType == 1)	str_copy(NameSkin, "cammostripes", sizeof(NameSkin));
	}
	else if(BotType == BOT_L3MONSTER)
	{
		if(!BotSubType) str_copy(NameSkin, "twintri", sizeof(NameSkin));
		else if(BotSubType == 1) str_copy(NameSkin, "coala", sizeof(NameSkin));
	}
	else if(BotType == BOT_NPC)
	{
		if(!BotSubType)	str_copy(NameSkin, "cammo", sizeof(NameSkin));
		else if(BotSubType == 1) str_copy(NameSkin, "cammostripes", sizeof(NameSkin));
	}
	else if(BotType == BOT_BOSSSLIME)
	{
		if(!BotSubType)	str_copy(NameSkin, "twinbop", sizeof(NameSkin));
		else if(BotSubType == 1) str_copy(NameSkin, "cammostripes", sizeof(NameSkin));
	}
	else if(BotType == BOT_FARMER)
	{
		str_copy(NameSkin, "redbopp", sizeof(NameSkin));
	}
	else if(BotType == BOT_NPCW)
	{
		if(BotSubType == 0) str_copy(NameSkin, "saddo", sizeof(NameSkin));
		else if(BotSubType == 1) str_copy(NameSkin, "twinbop", sizeof(NameSkin));
		else str_copy(NameSkin, "coala", sizeof(NameSkin));
	}
	
    Server()->ResetBotInfo(ClientID, BotType, BotSubType);
    str_copy(m_apPlayers[ClientID]->m_TeeInfos.m_SkinName, NameSkin, sizeof(m_apPlayers[ClientID]->m_TeeInfos.m_SkinName));
    m_apPlayers[ClientID]->m_TeeInfos.m_UseCustomColor = false;
    m_pController->OnPlayerInfoChange(m_apPlayers[ClientID]);
}

void CGameContext::CreateBot(int ClientID, int BotType, int BotSubType)
{
    int BotClientID = g_Config.m_SvMaxClients-MAX_BOTS+ClientID;
    if (m_apPlayers[BotClientID])
		return;

	m_apPlayers[BotClientID] = new(BotClientID) CPlayer(this, BotClientID, TEAM_RED);
	m_apPlayers[BotClientID]->SetBotType(BotType);
	m_apPlayers[BotClientID]->SetBotSubType(BotSubType);
	
	Server()->InitClientBot(BotClientID);
}

void CGameContext::DeleteBotBoss() { Server()->Kick(BOSSID, "pizdyi"); }

void CGameContext::SendChatClan(int ClanID, const char* pText, ...)
{	
	dynamic_string Buffer;
	
	va_list VarArgs;
	va_start(VarArgs, pText);
	
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientID = -1;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && Server()->GetClanID(i) && Server()->GetClanID(i) == ClanID)
		{	
			Buffer.clear();
			Server()->Localization()->Format_VL(Buffer, m_apPlayers[i]->GetLanguage(), pText, VarArgs);
			
			Msg.m_pMessage = Buffer.buffer();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
		}
	}
	Buffer.clear();
	va_end(VarArgs);
}

const char* CGameContext::LevelString(int max, int value, int step, char ch1, char ch2) 
{
    if (value < 0) value = 0;
    if (value > max) value = max;

    int size = 2 + max / step + 1;
    char *Buf = new char[size];
    Buf[0] = '[';
    Buf[size - 2] = ']';
    Buf[size - 1] = '\0';

    int a = value / step;
    int b = (max - value) / step;
    int i = 1;

    for (int ai = 0; ai < a; ai++, i++) 
    {
        Buf[i] = ch1;
    }

    for (int bi = 0; bi < b || i < size - 2; bi++, i++) 
    {
        Buf[i] = ch2;
    }
    return Buf;
}

const char* CGameContext::LocalizeText(int ClientID, const char* pText)
{
	return Server()->Localization()->Localize(m_apPlayers[ClientID]->GetLanguage(), _(pText));
}

/* Предметы все функции */
void CGameContext::UseItem(int ClientID, int ItemID, int Count, int Type)
{
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer || !pPlayer->GetCharacter())
		return;

	if(Type == USEDUSE)
	{
		if(ItemID == CLANTICKET)
			return SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("用法: /newclan <公会名> - 创建一个新的公会"), NULL);

		int PackOne = 0;
		for(int i = 0; i < Count; ++i)
		{
			if(ItemID == MONEYBAG)
			{
				PackOne += rand()%20000+5;
				if(i == Count-1)
				{
					int GetGold = PackOne/10000;
					int GetSilv = PackOne - GetGold*10000;

					SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 使用了物品:{str:used} x{int:num} 而且获得了 {int:pvar} 黄金与 {int:pvars} 白银"), 
						"name", Server()->ClientName(ClientID), "used", Server()->GetItemName(ClientID, ItemID, false), "num", &Count, "pvar", &GetGold, "pvars", &GetSilv , NULL);	
				
					pPlayer->MoneyAdd(PackOne);
				}
			}
			else if(ItemID == TOMATE)
			{
				PackOne += 15;
				if(i == Count-1)
				{
					SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 使用了物品:{str:used} x{int:num} 而且获得了 {int:pvars} 经验"), 
						"name", Server()->ClientName(ClientID), "used", Server()->GetItemName(ClientID, ItemID, false), "num", &Count, "pvars", &PackOne , NULL);	
				
					pPlayer->ExpAdd(PackOne, false);
				}
			}
			else if(ItemID == POTATO)
			{
				PackOne += 25;
				if(i == Count-1)
				{
					SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 使用了物品:{str:used} x{int:num} 而且获得了 {int:pvars} 经验"), 
						"name", Server()->ClientName(ClientID), "used", Server()->GetItemName(ClientID, ItemID, false), "num", &Count, "pvars", &PackOne , NULL);	
				
					pPlayer->ExpAdd(PackOne, false);
				}
			}
			else if(ItemID == CARROT)
			{
				PackOne += 10;
				if(i == Count-1)
				{
					SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 使用了物品:{str:used} x{int:num} 而且获得了 {int:pvars} 经验"), 
						"name", Server()->ClientName(ClientID), "used", Server()->GetItemName(ClientID, ItemID, false), "num", &Count, "pvars", &PackOne , NULL);	
				
					pPlayer->ExpAdd(PackOne, false);
				}
			}
			else if(ItemID == CLANBOXEXP)
			{
				if(!Server()->GetClanID(ClientID)) break;
				PackOne += rand()%20000+5;
				if(i == Count-1)
				{
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你使用了物品:{str:items}x{int:num}"), "items", Server()->GetItemName(ClientID, ItemID), "num", &Count, NULL);
					Server()->InitClanID(Server()->GetClanID(ClientID), PLUS, "Exp", PackOne, true);
				}
			}
			else if(ItemID == CLANTICKET)
			{		
				SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("用法: /newclan <公会名> - 创建一个新的公会"), NULL);	
				break;
			}
			else if(ItemID == RESETINGSKILL)
			{		
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 使用了物品:{str:items}x{int:num}"), "name", Server()->ClientName(ClientID), "items", Server()->GetItemName(ClientID, ItemID, false), "num", &Count, NULL);
				m_apPlayers[ClientID]->ResetSkill(ClientID);
			}
			else if(ItemID ==  RESETINGUPGRADE)
			{		
				SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 使用了物品:{str:items}x{int:num}"), "name", Server()->ClientName(ClientID), "items", Server()->GetItemName(ClientID, ItemID, false), "num", &Count, NULL);
				m_apPlayers[ClientID]->ResetUpgrade(ClientID);
			}
			else if(ItemID ==  BOOKEXPMIN)
			{		
				if(i == Count-1)
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你使用了物品:{str:items}x{int:num}"), "items", Server()->GetItemName(ClientID, ItemID), "num", &Count, NULL);
			
				m_apPlayers[ClientID]->m_ExperienceAdd += 600*Server()->TickSpeed();
			}
			else if(ItemID ==  BOOKMONEYMIN)
			{		
				if(i == Count-1)
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你使用了物品:{str:items}x{int:num}"), "items", Server()->GetItemName(ClientID, ItemID), "num", &Count, NULL);
				
				m_apPlayers[ClientID]->m_MoneyAdd += 600*Server()->TickSpeed();
			}
			else if(ItemID ==  SKILLUPBOX)
			{		
				m_apPlayers[ClientID]->AccUpgrade.Upgrade += 20;
				m_apPlayers[ClientID]->AccUpgrade.SkillPoint += 10;
				if(i == Count-1)
					SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你使用了物品:{str:items}x{int:num}"), "items", Server()->GetItemName(ClientID, ItemID), "num", &Count, NULL);

				UpdateUpgrades(ClientID);
			}
			else if(ItemID == VIPPACKAGE)
			{		
				Count = 1;
				SendMail(ClientID, "您购买了VIP，这是你的奖金!", SKILLUPBOX, 1);
				SendMail(ClientID, "您购买了VIP，这是你的奖金!", SANTIPVP, 1);
				SendMail(ClientID, "您购买了VIP，这是你的奖金!", X2MONEYEXPVIP, 1);
				SendMail(ClientID, "您购买了VIP，这是你的奖金!", SPECSNAPDRAW, 1);
				SendMail(ClientID, "您购买了VIP，这是你的奖金!", MONEYBAG, 10000);
				UpdateStats(ClientID);
				break;
			}
			else if(ItemID == RANDOMCRAFTITEM || ItemID == EVENTBOX || ItemID == FARMBOX)
			{
				Count = 1;
				m_apPlayers[ClientID]->m_OpenBox = 210;
				m_apPlayers[ClientID]->m_OpenBoxType = ItemID;
				break;
			}
			else 
				break;
		}
		dbg_msg("used", "%s use item %s:%d (count: %d)", Server()->ClientName(ClientID), Server()->GetItemName(ClientID, ItemID, false), ItemID, Count);
		return;
	}
	if(Type == USEDSELL)
	{
		int NeedMoney = (int)(Server()->GetItemPrice(ClientID, ItemID, 1)/2);
		NeedMoney = NeedMoney*Count;
		pPlayer->AccData.Gold += NeedMoney;
		
		SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你卖出了:{str:name}x{int:count}"), "name", Server()->GetItemName(ClientID, ItemID), "count", &Count, NULL);							
		SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("因此你获得了 {int:count} 黄金"), "count", &NeedMoney, NULL);							
		
		if(!Server()->GetItemCount(ClientID, ItemID))
		{
			switch(ItemID)
			{
				case IGUN:	pPlayer->GetCharacter()->RemoveGun(WEAPON_GUN); break;
				case ISHOTGUN: pPlayer->GetCharacter()->RemoveGun(WEAPON_SHOTGUN); break;
				case IGRENADE: pPlayer->GetCharacter()->RemoveGun(WEAPON_GRENADE); break;
				case ILASER: pPlayer->GetCharacter()->RemoveGun(WEAPON_RIFLE); break;
			}
		}

		dbg_msg("sell", "%s sell item %s:%d (count: %d)", Server()->ClientName(ClientID), Server()->GetItemName(ClientID, ItemID, false), ItemID, Count);
		Server()->SetMaterials(1, Server()->GetMaterials(1)+NeedMoney);

		UpdateStats(ClientID);
		ResetVotes(ClientID, AUTH);
		return;	
	}
	if(Type == USEDDROP)
	{
		int ClinID = -1;	
		if(ItemID == RANDOMCRAFTITEM || ItemID == POTATO || ItemID == TOMATE || ItemID == CARROT)
			ClinID = ClientID;

		if(Server()->GetItemType(ClientID, ItemID) == 15 || Server()->GetItemType(ClientID, ItemID) == 16)
			m_pController->OnPlayerInfoChange(pPlayer);

		if(!Server()->GetItemCount(ClientID, ItemID))
		{
			switch(ItemID)
			{
				case IGUN:	pPlayer->GetCharacter()->RemoveGun(WEAPON_GUN); break;
				case ISHOTGUN: pPlayer->GetCharacter()->RemoveGun(WEAPON_SHOTGUN); break;
				case IGRENADE: pPlayer->GetCharacter()->RemoveGun(WEAPON_GRENADE); break;
				case ILASER: pPlayer->GetCharacter()->RemoveGun(WEAPON_RIFLE); break;
			}
		}
		m_apPlayers[ClientID]->GetCharacter()->CreateDropItem(ItemID, Count, ClinID);

		dbg_msg("drop", "%s drop item %s:%d (count: %d)", Server()->ClientName(ClientID), Server()->GetItemName(ClientID, ItemID, false), ItemID, Count);
		SendChatTarget_Localization(ClientID, CHATCATEGORY_DEFAULT, _("你丢出了物品:{str:items}x{int:counts}"), "items", Server()->GetItemName(ClientID, ItemID), "counts", &Count, NULL);				
		return;
	}
	return;
}

/* Босс все функции */
void CGameContext::StartBoss(int ClientID, int WaitTime, int BossType)
{	
	if(!m_apPlayers[ClientID] || !m_apPlayers[ClientID]->GetCharacter())
		return;

	if(m_apPlayers[ClientID]->m_JailTick || m_apPlayers[ClientID]->m_Search)
		return 	SendBroadcast_Localization(ClientID, 250, 150, _("你被通缉了，不能进入Boss房间."));
		
	if(m_BossStartTick || m_BossStart)
		return 	SendBroadcast_Localization(ClientID, 250, 150, _("Boss房发起的Boss战即将开始. 进入Boss房以进入Boss战."));
	
	if (!m_apPlayers[BOSSID])
	{
		m_apPlayers[BOSSID] = new(BOSSID) CPlayer(this, BOSSID, TEAM_RED);
		m_apPlayers[BOSSID]->SetBotType(BossType);
		m_apPlayers[BOSSID]->SetBotSubType(g_Config.m_SvCityStart);
		
		Server()->InitClientBot(BOSSID);
	}
	else
	{
		SendBroadcast_Localization(ClientID, 190, 150, _("请创建一个新的请求(/createboss)."));
		DeleteBotBoss();
		return;
	}
			
	m_BossStartTick = Server()->TickSpeed()*WaitTime;
	m_BossType = BossType;
	
	m_apPlayers[ClientID]->m_InBossed = true;
	m_apPlayers[ClientID]->GetCharacter()->Die(ClientID, WEAPON_WORLD);

	SendChatTarget(-1, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
	SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("{str:name} 创建了需求，在Boss房(boss room) {str:names}"), "name", Server()->ClientName(ClientID), "names", GetBossName(m_BossType), NULL);
	SendChatTarget_Localization(-1, CHATCATEGORY_DEFAULT, _("欲加入的玩家, 你需要进入Boss房(boss room)"), "name", Server()->ClientName(ClientID), "names", GetBossName(m_BossType), NULL);
	SendChatTarget(-1, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
}

void CGameContext::EnterBoss(int ClientID, int BossType)
{	
	if(!m_apPlayers[ClientID] || !m_apPlayers[ClientID]->GetCharacter())
		return;
		
	if(m_apPlayers[ClientID]->m_JailTick || m_apPlayers[ClientID]->m_Search)
		return 	SendBroadcast_Localization(ClientID, 250, 150, _("你被通缉了，不能进入Boss房间."));
		
	if(!m_BossStartTick && m_BossStart)
		return 	SendBroadcast_Localization(ClientID, 250, 150, _("Boss战进行中. 等待其结束或者创建新的Boss."));
			
	if(!m_BossStartTick && !m_BossStart)
		return 	SendBroadcast_Localization(ClientID, 250, 150, _("需要创建请求(/createboss)."));

	m_apPlayers[ClientID]->m_InBossed = true;	
	if(Server()->GetClanID(ClientID) > 0)
	{
		for(int i = 0; i < MAX_NOBOT; ++i)
		{
			if(i != ClientID && m_apPlayers[i] && m_apPlayers[i]->m_InBossed)
			{
				int Get = m_apPlayers[i]->AccData.Level*6;
				SendChatTarget_Localization(i, CHATCATEGORY_DEFAULT, _("[公会] Relevance 增加了 +{int:count}"), "count", &Get, NULL);			
				Server()->InitClanID(Server()->GetClanID(ClientID), PLUS, "Relevance", Get, true);
			}
		}
	}
	m_apPlayers[ClientID]->GetCharacter()->Die(ClientID, WEAPON_WORLD);
}

int CGameContext::GetBossLeveling()
{
	int Count = 0;
	for(int i = 0; i < MAX_NOBOT; ++i)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->m_InBossed)
			Count += m_apPlayers[i]->AccData.Level;
	}
	return Count;
}

int CGameContext::GetBossCount()
{
	int Count = 0;
	for(int i = 0; i < MAX_NOBOT; ++i)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->m_InBossed)
			Count++;
	}
	return Count;
}

const char *CGameContext::GetBossName(int BossType)
{
	if(BossType == BOT_BOSSSLIME)
	{
		if(!g_Config.m_SvCityStart)
			return "Slime";
		else if(g_Config.m_SvCityStart == 1)
			return "Vampire";
		else
			return "(unknow)";
	}
	else
		return "(unknow)";

}
