#include "plugin.h"
#include "..\injector\assembly.hpp"
#include "CTxdStore.h"
#include "CVisibilityPlugins.h"
#include "CWeather.h"
#include "CTimer.h"
#include "CCamera.h"
#include "CClock.h"
#include "CScene.h"
#include "CTimeCycle.h"
#include "CCheat.h"
#include "CGame.h"
#include "CGeneral.h"
#include "CSprite.h"
#include "CPickups.h"
#include "CCutsceneMgr.h"
#include "IniReader/IniReader.h"

using namespace plugin;
using namespace std;
using namespace injector;

CdeclEvent <AddressList<0x53DFA0, H_CALL>, PRIORITY_AFTER, ArgPickNone, void()> renderClouds;
CdeclEvent <AddressList<0x53C0DA, H_CALL>, PRIORITY_AFTER, ArgPickNone, void()> updateCurrentTimecycle;

fstream lg;
const float magic = 1.6666667f;

RpAtomic *skyAtomic = nullptr;
RwFrame *skyFrame = nullptr;

class Skybox {
public:
	bool inUse;
	RwTexture *tex;
	float rot;
	Skybox() {
		inUse = false;
		tex = nullptr;
		rot = 0.0f;
	}
};

Skybox *skyboxes[21];

bool changeWeather = true;
float testInterp = 0.0f;
bool usingInterp = false;
bool processedFirst = false;
float inCityFactor = 0.0f;
const int WEATHER_FOR_STARS = 20;

RwV3d oldSkyboxScale;
RwV3d newSkyboxScale;
RwV3d starsSkyboxScale;
RwV3d cloudsRotationVector;
RwV3d starsRotationVector;

bool ReadSettingsFile() {
	lg << "Reading settings...\n";
	lg.flush();
	int read = 0;
	ifstream stream("realskybox/skyboxes.dat");
	for (string line; getline(stream, line); ) {
		if (line[0] != ';' && line[0] != '#') {

			if (line.compare("skytexs") == 0) {
				int weatherId = 0;
				char textureName[32];
				lg << "Reading skytexs...\n";
				lg.flush();
				while (getline(stream, line) && line.compare("end")) {
					if (line[0] != ';' && line[0] != '#') {
						if (sscanf(line.c_str(), "%d, %s", &weatherId, &textureName) == 2 && weatherId <= WEATHER_FOR_STARS)
						{
							skyboxes[weatherId]->tex = CallAndReturn<RwTexture*, 0x7F3AC0, const char*, const char*>(textureName, 0); //RwTexture *__cdecl RwReadTexture(char *name, char *maskName)
							if (skyboxes[weatherId]->tex) skyboxes[weatherId]->tex->filterAddressing = 2;
							read++;
						}
						else
						{
							lg << "'" << line << "' ---------- NOT VALID\n";
							lg.flush();
						}
					}
				}
			}
		}
	}
	lg << "Done reading settings.\n";
	lg.flush();
	return (read > 0);
}

static void StoreSkyboxModel() 
{
	string fileName = "realskybox/skybox.dff";

	RwStream *stream = RwStreamOpen(RwStreamType::rwSTREAMFILENAME, RwStreamAccessType::rwSTREAMREAD, &fileName[0]);

	if (stream)
	{
		if (RwStreamFindChunk(stream, 0x10, 0, 0)) //RpClump
		{
			RpClump *clump = RpClumpStreamRead(stream);
			if (clump)
			{
				RpAtomic *atomic = GetFirstAtomic(clump);
				atomic->renderCallBack = AtomicDefaultRenderCallBack;

				if (atomic)
				{
					skyAtomic = atomic;
					skyFrame = RwFrameCreate();
					RpAtomicSetFrame(skyAtomic, skyFrame);
					RwFrameUpdateObjects(skyFrame);
				}
				else lg << "Error: Can't find atomic: << " << fileName << endl;
			}
			else lg << "Error: Can't find clump: << " << fileName << endl;
		}
		else lg << "Error: Invalid DFF format: << " << fileName << endl;
	}
	else lg << "Error: Can't open " << fileName << endl;
	RwStreamClose(stream, 0);
	lg.flush();
}

void SetInUseForThisTexture(RwTexture *tex)
{
	for (int i = 0; i <= eWeatherType::WEATHER_UNDERWATER; ++i)
	{
		if (tex == skyboxes[i]->tex) skyboxes[i]->inUse = true;
	}
}

void SetRotationForThisTexture(RwTexture *tex, float rot)
{
	for (int i = 0; i <= eWeatherType::WEATHER_UNDERWATER; ++i)
	{
		if (tex == skyboxes[i]->tex) skyboxes[i]->rot = rot;
	}
}

bool NoSunriseWeather(eWeatherType id)
{
	return (id == eWeatherType::WEATHER_CLOUDY_COUNTRYSIDE || id == eWeatherType::WEATHER_CLOUDY_LA || id == eWeatherType::WEATHER_CLOUDY_SF || id == eWeatherType::WEATHER_CLOUDY_VEGAS ||
			id == eWeatherType::WEATHER_RAINY_COUNTRYSIDE || id == eWeatherType::WEATHER_RAINY_SF || id == eWeatherType::WEATHER_FOGGY_SF);
}

float increaseRot = 0.0f;

float& currentFarClip = *(float*)0xB7C4F0;
uint16_t& currentSkyBottomRed = *(uint16_t*)0xB7C4CA;
uint16_t& currentSkyTopRed = *(uint16_t*)0xB7C4C4;
uint16_t& currentSkyBottomGreen = *(uint16_t*)0xB7C4CC;
uint16_t& currentSkyTopGreen = *(uint16_t*)0xB7C4C6;
uint16_t& currentSkyBottomBlue = *(uint16_t*)0xB7C4CE;
uint16_t& currentSkyTopBlue = *(uint16_t*)0xB7C4C8;

float GetDayNightBalance() {
	return plugin::CallAndReturn<float, 0x6FAB30>();
}

bool sunReflectionChanged = false;
bool skyboxDrawAfter = true;
float lastFarClip = 0.0f;
float minFarPlane = 1100.0f;
float gameDefaultFogDensity = 1.0f;
float fogDensityDefault = 0.0012f;
float fogDensity = fogDensityDefault;
int skyboxFogType = 2;
float cloudsRotationSpeed = 0.002f;
float starsRotationSpeed = 0.0002f;
float skyboxSizeXY = 0.4f;
float skyboxSizeZ = 0.2f;
float cloudsMultBrightness = 0.4f;
float cloudsNightDarkLimit = 0.8f;
float cloudsMinBrightness = 0.05f;
float cloudsCityOrange = 1.0f;
float starsCityAlphaRemove = 0.8f;
float cloudsMultSunrise = 2.5f;

void RenderSkybox()
{
	if (CTimeCycle::m_bExtraColourOn == 0 && CWeather::OldWeatherType != eWeatherType::WEATHER_UNDERWATER && CWeather::NewWeatherType != eWeatherType::WEATHER_UNDERWATER && CWeather::ForcedWeatherType != eWeatherType::WEATHER_UNDERWATER)
	{
		if (!processedFirst)
		{
			lg << "Rendering OK.\n";
			lg.flush();
			inCityFactor = (CWeather::WeatherRegion == eWeatherRegion::WEATHER_REGION_DEFAULT || CWeather::WeatherRegion == eWeatherRegion::WEATHER_REGION_DESERT) ? 0.0f : 1.0f;
			processedFirst = true;
		}

		// Tweak weather type IDs
		int oldWeatherType = CWeather::OldWeatherType;
		int newWeatherType = CWeather::NewWeatherType;
		if (oldWeatherType > eWeatherType::WEATHER_UNDERWATER) oldWeatherType = eWeatherType::WEATHER_SUNNY_LA;
		if (newWeatherType > eWeatherType::WEATHER_UNDERWATER) newWeatherType = eWeatherType::WEATHER_SUNNY_LA;

		// Decrease increased rot
		if (increaseRot > 0.0f) {
			increaseRot -= pow(0.07f, 2) * CTimer::ms_fTimeStep * magic;
			if (increaseRot < 0.0f) increaseRot = 0.0f;
		}

		// Tweak by distance
		float farPlane = Scene.m_pRwCamera->farPlane;
		float goodDistanceFactor = (farPlane - 1000.0f) / 1000.0f; //  if farPlane is 2000.0, goodDistanceFactor is 2.0
		if (goodDistanceFactor < 0.01f) goodDistanceFactor = 0.01f;

		if (skyboxFogType <= 1) //linear
		{
			fogDensity = gameDefaultFogDensity;
		}
		else
		{
			fogDensity = fogDensityDefault / goodDistanceFactor;
			if (CWeather::UnderWaterness > 0.4f)
			{
				fogDensity *= 1.0f + ((CWeather::UnderWaterness - 0.4f) * 100.0f);
				fogDensity *= 0.1f;
			}
		}

		oldSkyboxScale.x = skyboxSizeXY * goodDistanceFactor;
		oldSkyboxScale.y = skyboxSizeXY * goodDistanceFactor;
		oldSkyboxScale.z = skyboxSizeZ * goodDistanceFactor;

		newSkyboxScale.x = oldSkyboxScale.x * 1.05f;
		newSkyboxScale.y = oldSkyboxScale.y * 1.05f;
		newSkyboxScale.z = oldSkyboxScale.z * 1.05f;

		starsSkyboxScale.x = newSkyboxScale.x * 1.05f;
		starsSkyboxScale.y = newSkyboxScale.y * 1.05f;
		starsSkyboxScale.z = newSkyboxScale.z * 1.05f;

		// Get essentials
		float oldAlpha = (1.0f - CWeather::InterpolationValue) * 255.0f;
		float newAlpha = CWeather::InterpolationValue * 255.0f;
		float dayNightBalance = GetDayNightBalance();

		// Get position
		CVector *camPos = &TheCamera.GetPosition();

		// Set random rotation to skyboxes not in use
		for (int i = 0; i <= eWeatherType::WEATHER_UNDERWATER; ++i)
		{
			if (!skyboxes[i]->inUse)
			{
				if (i == WEATHER_FOR_STARS)
				{
					skyboxes[i]->rot = CClock::ms_nGameClockMonth * 30.0f; // stars always starts with rotation based on month
				}
				else
				{
					skyboxes[i]->rot = CGeneral::GetRandomNumberInRange(0.0f, 360.0f);
				}
			}
			skyboxes[i]->inUse = false; // reset flag
		}

		// Consider in city
		if (CWeather::WeatherRegion == eWeatherRegion::WEATHER_REGION_DEFAULT || CWeather::WeatherRegion == eWeatherRegion::WEATHER_REGION_DESERT)
		{
			inCityFactor -= 0.001f * CTimer::ms_fTimeStep * magic;
			if (inCityFactor < 0.0f) inCityFactor = 0.0f;
		}
		else
		{
			inCityFactor += 0.001f * CTimer::ms_fTimeStep * magic;
			if (inCityFactor > 1.0f) inCityFactor = 1.0f;
		}

		// Get stars alpha
		float starsAlpha = 0.0f;
		if (dayNightBalance > 0.0f)
		{
			float skyIllumination = (currentSkyBottomRed + currentSkyBottomGreen + currentSkyBottomBlue + currentSkyTopRed + currentSkyTopGreen + currentSkyTopBlue) / 255.0f;
			if (skyIllumination > 1.0f) skyIllumination = 1.0f;
			starsAlpha = (1.0f - skyIllumination) * dayNightBalance;
			starsAlpha -= 1.0f * (inCityFactor * (starsCityAlphaRemove / 2.0f));
		}

		// Next weather texture is different from current?
		bool newTexIsDifferent = (skyboxes[oldWeatherType]->tex != skyboxes[newWeatherType]->tex);
		if (!newTexIsDifferent)
		{
			oldAlpha += newAlpha;
			if (oldAlpha > 255.0f) oldAlpha = 255.0f;
		}

		// Process rotation
		if (CCheat::m_aCheatsActive[0xB]) // fast clock
		{
			skyboxes[oldWeatherType]->rot += 0.1f + increaseRot * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
			if (newTexIsDifferent) skyboxes[newWeatherType]->rot += (0.1f * 0.7f) + increaseRot * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
			skyboxes[WEATHER_FOR_STARS]->rot += 0.005f + increaseRot * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
		}
		else
		{
			skyboxes[oldWeatherType]->rot += (cloudsRotationSpeed * 0.5f) + increaseRot * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
			if (newTexIsDifferent) skyboxes[newWeatherType]->rot += (cloudsRotationSpeed * 0.5f * 0.7f) + increaseRot * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
			skyboxes[WEATHER_FOR_STARS]->rot += (starsRotationSpeed * 0.5f) + increaseRot * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
		}
		while (skyboxes[oldWeatherType]->rot > 360.0f) skyboxes[oldWeatherType]->rot -= 360.0f;
		while (skyboxes[newWeatherType]->rot > 360.0f) skyboxes[newWeatherType]->rot -= 360.0f;
		while (skyboxes[WEATHER_FOR_STARS]->rot > 360.0f) skyboxes[WEATHER_FOR_STARS]->rot -= 360.0f;
		SetRotationForThisTexture(skyboxes[oldWeatherType]->tex, skyboxes[oldWeatherType]->rot);
		if (newTexIsDifferent) SetRotationForThisTexture(skyboxes[newWeatherType]->tex, skyboxes[newWeatherType]->rot);

		// Get Ilumination
		float skyboxIllumination = ((currentSkyBottomRed + currentSkyBottomGreen + currentSkyBottomBlue) * cloudsMultBrightness) / 255.0f;
		if (skyboxIllumination > 1.0f) skyboxIllumination = 1.0f;
		if (dayNightBalance != 0.0f && inCityFactor != 0.0f) skyboxIllumination -= (dayNightBalance / 12.0f) * (1.0f - inCityFactor);
		float dayNightBalanceReverse = (1.0f - dayNightBalance);
		if (dayNightBalanceReverse < cloudsNightDarkLimit) dayNightBalanceReverse = cloudsNightDarkLimit;
		skyboxIllumination *= dayNightBalanceReverse;
		if (skyboxIllumination < cloudsMinBrightness) skyboxIllumination = cloudsMinBrightness;
		if (skyboxIllumination > 1.0f) skyboxIllumination = 1.0f;

		// Get color
		float sunriseFactor = 0.0f;
		float sunHorizonFactor = CTimeCycle::m_VectorToSun[CTimeCycle::m_CurrentStoredValue].z;
		if (sunHorizonFactor > 0.0f)
		{
			if (sunHorizonFactor > 0.2f) sunHorizonFactor -= (sunHorizonFactor - 0.2f) * 2.0f; // 0.0 - 0.2 - 0.0
			sunriseFactor = sunHorizonFactor * 10.0f;
			if (sunriseFactor > 1.0f) sunriseFactor = 1.0f;
			if (NoSunriseWeather((eWeatherType)oldWeatherType)) sunriseFactor -= (oldAlpha / 255.0f);
			if (NoSunriseWeather((eWeatherType)newWeatherType)) sunriseFactor -= (newAlpha / 255.0f);
			if (sunriseFactor > 0.0f)
			{
				sunriseFactor *= cloudsMultSunrise;
				if (sunHorizonFactor > 0.0f) sunriseFactor += (abs(1.0f - sunHorizonFactor) * sunHorizonFactor);
			}
			else
			{
				sunriseFactor = 0.0f;
			}
		}
		sunriseFactor += ((cloudsCityOrange / 4.0f) * inCityFactor) * dayNightBalance;

		RwRGBAReal skyboxColor
		{
			skyboxIllumination,
			(skyboxIllumination - (sunriseFactor / 16.0f)),
			(skyboxIllumination - (sunriseFactor / 10.0f))
		};

		// Start render
		RwEngineInstance->dOpenDevice.fpRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)1u);
		RwEngineInstance->dOpenDevice.fpRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)1u);
		RwEngineInstance->dOpenDevice.fpRenderStateSet(rwRENDERSTATEFOGTYPE, (void*)skyboxFogType);
		RwEngineInstance->dOpenDevice.fpRenderStateSet(rwRENDERSTATEFOGDENSITY, &fogDensity);

		// Render skyboxes
		if (starsAlpha > 0.0f) // Stars
		{
			RwFrameTranslate(skyFrame, &camPos->ToRwV3d(), rwCOMBINEREPLACE);
			RwFrameScale(skyFrame, &starsSkyboxScale, rwCOMBINEPRECONCAT);
			RwFrameRotate(skyFrame, (RwV3d*)0x008D2E18, skyboxes[WEATHER_FOR_STARS]->rot, rwCOMBINEPRECONCAT);
			RwFrameUpdateObjects(skyFrame);

			skyAtomic->geometry->matList.materials[0]->texture = skyboxes[WEATHER_FOR_STARS]->tex;
			skyboxes[WEATHER_FOR_STARS]->inUse = true;

			int finalAlpha = (int)(starsAlpha * 255.0f);
			if (skyboxFogType <= 1 && CWeather::UnderWaterness > 0.4f) finalAlpha /= 1.0f + ((CWeather::UnderWaterness - 0.4f) * 100.0f);

			SetFullAmbient();
			DeActivateDirectional();
			RpLight *pDirect = *(RpLight **)0xC886EC;
			pDirect->object.object.flags = 0; // same as DeActivateDirectional(); (trying to fix directional lights SAMP bug)
			CVisibilityPlugins::RenderAlphaAtomic(skyAtomic, finalAlpha);
		}

		if (skyboxes[oldWeatherType]->tex && oldAlpha > 0.0f) // Old (current)
		{
			RwFrameTranslate(skyFrame, &camPos->ToRwV3d(), rwCOMBINEREPLACE);
			RwFrameScale(skyFrame, &oldSkyboxScale, rwCOMBINEPRECONCAT);
			RwFrameRotate(skyFrame, (RwV3d*)0x008D2E18, skyboxes[oldWeatherType]->rot, rwCOMBINEPRECONCAT);
			RwFrameUpdateObjects(skyFrame);

			skyAtomic->geometry->matList.materials[0]->texture = skyboxes[oldWeatherType]->tex;
			SetInUseForThisTexture(skyboxes[oldWeatherType]->tex);

			int finalAlpha = (int)oldAlpha;
			if (skyboxFogType <= 1 && CWeather::UnderWaterness > 0.4f) finalAlpha /= 1.0f + ((CWeather::UnderWaterness - 0.4f) * 100.0f);

			SetAmbientColours(&skyboxColor);
			DeActivateDirectional();
			RpLight *pDirect = *(RpLight **)0xC886EC;
			pDirect->object.object.flags = 0; // same as DeActivateDirectional(); (trying to fix directional lights SAMP bug)
			CVisibilityPlugins::RenderAlphaAtomic(skyAtomic, finalAlpha);
		}

		if (newTexIsDifferent && skyboxes[newWeatherType]->tex && newAlpha > 0.0f) // New (next)
		{
			RwFrameTranslate(skyFrame, &camPos->ToRwV3d(), rwCOMBINEREPLACE);
			RwFrameScale(skyFrame, &newSkyboxScale, rwCOMBINEPRECONCAT);
			RwFrameRotate(skyFrame, (RwV3d*)0x008D2E18, skyboxes[newWeatherType]->rot, rwCOMBINEPRECONCAT);
			RwFrameUpdateObjects(skyFrame);

			skyAtomic->geometry->matList.materials[0]->texture = skyboxes[newWeatherType]->tex;
			SetInUseForThisTexture(skyboxes[newWeatherType]->tex);

			int finalAlpha = (int)newAlpha;
			if (skyboxFogType <= 1 && CWeather::UnderWaterness > 0.4f) finalAlpha /= 1.0f + ((CWeather::UnderWaterness - 0.4f) * 100.0f);

			SetAmbientColours(&skyboxColor);
			DeActivateDirectional();
			RpLight *pDirect = *(RpLight **)0xC886EC;
			pDirect->object.object.flags = 0; // same as DeActivateDirectional(); (trying to fix directional lights SAMP bug)
			CVisibilityPlugins::RenderAlphaAtomic(skyAtomic, finalAlpha);
		}

		// Finish render
		RwEngineInstance->dOpenDevice.fpRenderStateSet(rwRENDERSTATEFOGDENSITY, &gameDefaultFogDensity);
		RwEngineInstance->dOpenDevice.fpRenderStateSet(rwRENDERSTATEFOGTYPE, (void*)RwFogType::rwFOGTYPELINEAR);
		RwEngineInstance->dOpenDevice.fpRenderStateSet(rwRENDERSTATEFOGENABLE, 0);
	}
}

class RealSkybox
{
public:
    RealSkybox()
	{
		lg.open("RealSkybox.SA.log", fstream::out | fstream::trunc);
		lg << "RealSkybox v1.3.2 by Junior_Djjr - MixMods.com.br" << endl;
		lg.flush();

		Events::initScriptsEvent += []
		{
			CIniReader ini("RealSkybox.SA.ini");

			if (ini.data.size() > 0)
			{
				if (ini.ReadInteger("Game tweaks", "NoLowClouds", 0) == 1) {
					MakeNOP(0x53E1B4, 5, true); // disable low clouds
				}
				if (ini.ReadInteger("Game tweaks", "NoHorizonClouds", 0) == 1) {
					MakeJMP(0x714145, 0x71422A, true); // disable horizon clouds
				}
				if (ini.ReadInteger("Game tweaks", "NoVolumetricClouds", 0) == 1) {
					MakeNOP(0x53E121, 5, true); // disable volumetric clouds
				}
				if (ini.ReadInteger("Game tweaks", "NoVanillaStars", 0) == 1) {
					MakeJMP(0x7143AE, 0x714639, true); // disable stars
					MakeJMP(0x713D73, 0x71401E, true); // disable stars
				}
				minFarPlane = ini.ReadFloat("Game tweaks", "MinDrawDistance", minFarPlane);
				skyboxDrawAfter = ini.ReadInteger("Skybox", "SkyboxDrawAfter", skyboxDrawAfter);
				fogDensityDefault = ini.ReadFloat("Skybox", "SkyboxFogDistance", fogDensityDefault);
				skyboxFogType = ini.ReadInteger("Skybox", "SkyboxFogType", skyboxFogType);
				cloudsRotationSpeed = ini.ReadFloat("Skybox", "CloudsRotationSpeed", cloudsRotationSpeed);
				starsRotationSpeed = ini.ReadFloat("Skybox", "StarsRotationSpeed", starsRotationSpeed);
				skyboxSizeXY = ini.ReadFloat("Skybox", "SkyboxSizeXY", skyboxSizeXY);
				skyboxSizeZ = ini.ReadFloat("Skybox", "SkyboxSizeZ", skyboxSizeZ);
				cloudsMultBrightness = ini.ReadFloat("Skybox", "CloudsMultBrightness", cloudsMultBrightness);
				cloudsNightDarkLimit = ini.ReadFloat("Skybox", "CloudsNightDarkLimit", cloudsNightDarkLimit);
				cloudsMinBrightness = ini.ReadFloat("Skybox", "CloudsMinBrightness", cloudsMinBrightness);
				cloudsCityOrange = ini.ReadFloat("Skybox", "CloudsCityOrange", cloudsCityOrange);
				starsCityAlphaRemove = ini.ReadFloat("Skybox", "StarsCityAlphaRemove", starsCityAlphaRemove);
				cloudsMultSunrise = ini.ReadFloat("Skybox", "CloudsMultSunrise", cloudsMultSunrise);
			}
			else
			{
				lg << "ERROR: Fail to read .ini file.\n";
				lg.flush();
			}

			if (processedFirst) return;


			// Store skyboxes
			for (int i = 0; i < 21; ++i) {skyboxes[i] = new Skybox();}
			StoreSkyboxModel();
			int txdSlot = CTxdStore::AddTxdSlot("realskybox");
			CTxdStore::LoadTxd(txdSlot, "realskybox/skyboxes.txd");
			CTxdStore::AddRef(txdSlot);
			CTxdStore::SetCurrentTxd(txdSlot);
			if (!ReadSettingsFile())
			{
				CTxdStore::PopCurrentTxd();
				lg << "ERROR: Fail to read .dat file.\n";
				lg.flush();
				return;
			}
			CTxdStore::PopCurrentTxd();

			sunReflectionChanged = (ReadMemory<uint32_t>(0x6FC051 + 2, true) > 28800) ? true : false;

			lg << "Load done" << endl;
			lg.flush();

			if (skyboxDrawAfter)
			{
				renderClouds.after += [] { RenderSkybox(); };
			}
			else
			{
				renderClouds.before += [] { RenderSkybox(); };
			}

			updateCurrentTimecycle.after += []
			{
				if (CTimeCycle::m_bExtraColourOn == 0 && CWeather::OldWeatherType != eWeatherType::WEATHER_UNDERWATER && CWeather::NewWeatherType != eWeatherType::WEATHER_UNDERWATER && CWeather::ForcedWeatherType != eWeatherType::WEATHER_UNDERWATER && CWeather::UnderWaterness < 0.4f)
				{
					// Min far clip
					if (currentFarClip < minFarPlane)
					{
						currentFarClip = minFarPlane; //float CTimeCycle::m_fCurrentFarClip
					}
					// Update far clip change
					if (lastFarClip != currentFarClip && !sunReflectionChanged)
					{
						WriteMemory<uint32_t>(0x6FC051 + 2, (28800 * (currentFarClip / 1000.0f)), true);
					}
					lastFarClip = currentFarClip;
				}
			};

			injector::MakeInline<0x4414DA, 0x4414DA + 7>([](injector::reg_pack& regs)
			{
				regs.edi = CClock::CurrentDay;

				int time = regs.ecx;

				// Set instantly, i.e. saving, wasted, busted, any situation where the game changes the time during a transition.
				if (TheCamera.m_fFadeAlpha > 200.0f)
				{
					int oldWeatherType = CWeather::OldWeatherType;
					int newWeatherType = CWeather::NewWeatherType;
					if (oldWeatherType > eWeatherType::WEATHER_UNDERWATER) oldWeatherType = eWeatherType::WEATHER_SUNNY_LA;
					if (newWeatherType > eWeatherType::WEATHER_UNDERWATER) newWeatherType = eWeatherType::WEATHER_SUNNY_LA;

					float fTime = log10((float)time) * 2.0f * CTimer::ms_fTimeStep * magic;
					skyboxes[oldWeatherType]->rot += 0.1f + fTime * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
					skyboxes[newWeatherType]->rot += (0.1f * 0.7f) + fTime * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
					skyboxes[WEATHER_FOR_STARS]->rot += 0.005f + fTime * CTimer::ms_fTimeScale * (CTimer::ms_fTimeStep * magic);
				}
				else
				{
					if (CTimeCycle::m_bExtraColourOn == 0 && CWeather::OldWeatherType != eWeatherType::WEATHER_UNDERWATER && CWeather::NewWeatherType != eWeatherType::WEATHER_UNDERWATER && CWeather::ForcedWeatherType != eWeatherType::WEATHER_UNDERWATER) {
						float fTime = log10((float)time) * CTimer::ms_fTimeStep * magic;

						float increaseLimitMin = 0.1f * CTimer::ms_fTimeStep * magic;
						if (fTime < increaseLimitMin) fTime = increaseLimitMin;

						increaseRot += fTime;

						float increaseLimitMax = fTime * 0.5f * CTimer::ms_fTimeStep * magic;
						if (increaseRot > increaseLimitMax) increaseRot = increaseLimitMax;
					}
				}
			});

		};

		
		/*Events::processScriptsEvent.after += []
		{
			// hour min
			if (GetKeyState('H') & 0x8000)
			{
				if (GetKeyState('Y') & 0x80)
				{
					CClock::ms_nGameClockHours++;
				}
				if (GetKeyState('N') & 0x80)
				{
					CClock::ms_nGameClockHours--;
				}
			}
			if (GetKeyState('M') & 0x8000)
			{
				if (GetKeyState('Y') & 0x80)
				{
					CClock::ms_nGameClockMinutes++;
				}
				if (GetKeyState('N') & 0x80)
				{
					CClock::ms_nGameClockMinutes--;
				}
			}

			// far clip
			if (GetKeyState('T') & 0x8000)
			{
				if (GetKeyState('Y') & 0x8000)
				{
					minFarPlane += 100.0f * CTimer::ms_fTimeStep;
					if (minFarPlane > 20000.0f) minFarPlane = 20000.0f;
					lg << minFarPlane << endl;
				}
				if (GetKeyState('N') & 0x8000)
				{
					minFarPlane -= 100.0f * CTimer::ms_fTimeStep;
					if (minFarPlane < 100.0f) minFarPlane = 100.0f;
					lg << minFarPlane << endl;
				}
			}

			// fog
			if (GetKeyState('F') & 0x8000)
			{
				if (GetKeyState('Y') & 0x8000)
				{
					fogDensityDefault += 0.0001f * CTimer::ms_fTimeStep;
					//if (fogDensityDefault > 1.0f) fogDensityDefault = 1.0f;
					lg << fogDensityDefault << endl;
				}
				if (GetKeyState('N') & 0x8000)
				{
					fogDensityDefault -= 0.0001f * CTimer::ms_fTimeStep;
					if (fogDensityDefault < 0.0000000f) fogDensityDefault = 0.0000000f;
					lg << fogDensityDefault << endl;
				}
			}

			// interp
			if (usingInterp)
			{
				CWeather::OldWeatherType = 0;
				CWeather::NewWeatherType = 8;
				CWeather::InterpolationValue = testInterp;
			}
			if (GetKeyState('I') & 0x8000)
			{
				if (GetKeyState('Y') & 0x8000)
				{
					testInterp += 0.1f * CTimer::ms_fTimeStep;
					if (testInterp > 1.0f) testInterp = 1.0f;
					usingInterp = true;
					lg << testInterp << endl;
				}
				if (GetKeyState('N') & 0x8000)
				{
					testInterp -= 0.1f * CTimer::ms_fTimeStep;
					if (testInterp < 0.0f) testInterp = 0.0f;
					usingInterp = true;
					lg << testInterp << endl;
				}
			}
		};*/
        
    }
} realSkybox;
