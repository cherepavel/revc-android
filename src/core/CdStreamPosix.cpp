#ifndef _WIN32
#include "common.h"
#include "crossplatform.h"
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#ifndef ANDROID
  #include <sys/statvfs.h>
#else
  #include <sys/vfs.h>
  #define statvfs statfs
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <stdarg.h>
#include <limits.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "CdStream.h"
#include "rwcore.h"
#include "MemoryMgr.h"

#define CDDEBUG(f, ...)   debug ("%s: " f "\n", "cdvd_stream", ## __VA_ARGS__)
#define CDTRACE(f, ...)   debug("%s: " f "\n", "cdvd_stream error", ## __VA_ARGS__)

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

struct CdReadInfo
{
	uint32 nSectorOffset;
	uint32 nSectorsToRead;
	void *pBuffer;
	bool bLocked;
	bool bReading;
	int32 nStatus;
#ifdef ONE_THREAD_PER_CHANNEL
	int8 nThreadStatus; // 0: created 1:priority set up 2:abort now
	pthread_t pChannelThread;
	sem_t *pStartSemaphore;
#endif
	//sem_t *pDoneSemaphore; // used for CdStreamSync
	pthread_mutex_t doneMutex;
	int32 hFile;
};

char gCdImageNames[MAX_CDIMAGES+1][64];
int32 gNumImages;
int32 gNumChannels;

int32 gImgFiles[MAX_CDIMAGES]; // -1: error 0:unused otherwise: fd
char *gImgNames[MAX_CDIMAGES];

#ifndef ONE_THREAD_PER_CHANNEL
pthread_t _gCdStreamThread;
//sem_t *gCdStreamSema; // released when we have new thing to read(so channel is set)
pthread_mutex_t cdStreamMutex;
int8 gCdStreamThreadStatus; // 0: created 1:priority set up 2:abort now
Queue gChannelRequestQ;
bool _gbCdStreamOverlapped;
#endif

CdReadInfo *gpReadInfo;

int32 lastPosnRead;

int _gdwCdStreamFlags;

void *CdStreamThread(void* channelId);

void
CdStreamInitThread(void)
{
	int status;
#ifndef ONE_THREAD_PER_CHANNEL
	gChannelRequestQ.items = (int32 *)calloc(gNumChannels + 1, sizeof(int32));
	gChannelRequestQ.head = 0;
	gChannelRequestQ.tail = 0;
	gChannelRequestQ.size = gNumChannels + 1;
	ASSERT(gChannelRequestQ.items != nil );
	//pthread_mutex_unlock(&cdStreamMutex);
#endif

	if ( gNumChannels > 0 )
	{
		for ( int32 i = 0; i < gNumChannels; i++ )
		{
			//pthread_mutex_unlock(&gpReadInfo[i].doneMutex);

#ifdef ONE_THREAD_PER_CHANNEL
			gpReadInfo[i].pStartSemaphore = RE3_SEM_OPEN("/semaphore_start%d", i);

			if (gpReadInfo[i].pStartSemaphore == SEM_FAILED)
			{
				CDTRACE("failed to create start semaphore");
				ASSERT(0);
				return;
			}
			gpReadInfo[i].nThreadStatus = 0;
			int *channelI = (int*)malloc(sizeof(int));
			*channelI = i;
			status = pthread_create(&gpReadInfo[i].pChannelThread, NULL, CdStreamThread, (void*)channelI);

			if (status == -1)
			{
				CDTRACE("failed to create sync thread");
				ASSERT(0);
				return;
			}
#endif
		}
	}

#ifndef ONE_THREAD_PER_CHANNEL
	debug("Using one streaming thread for all channels\n");
	gCdStreamThreadStatus = 0;
	status = pthread_create(&_gCdStreamThread, NULL, CdStreamThread, nil);

	if (status == -1)
	{
		CDTRACE("failed to create sync thread");
		ASSERT(0);
		return;
	}
#else
	debug("Using separate streaming threads for each channel\n");
#endif
}

void
CdStreamInit(int32 numChannels)
{
	struct statvfs fsInfo;
	
	char imgPath[MAX_PATH];
	
	if(getenv("GAMEFILES") == NULL)
	{
		char pwd[128];
		getcwd(pwd, 128);
		setenv("GAMEFILES", pwd, 1);
		printf("%s\n", pwd);
	}
	
	printf("FILES %s\n", getenv("GAMEFILES"));
	
	strcpy(imgPath, getenv("GAMEFILES"));
	strcat(imgPath, "/models/gta3.img");
	printf("%s\n", imgPath);
	if((statvfs(imgPath, &fsInfo)) < 0)
	{
		CDTRACE("can't get filesystem info");
		ASSERT(0);
		return;
	}
#ifdef ANDROID
	_gdwCdStreamFlags = O_RDONLY;
#elif defined __linux__
	_gdwCdStreamFlags = O_RDONLY | O_NOATIME;
#else
	_gdwCdStreamFlags = O_RDONLY;
#endif
	// People say it's slower
/*
	if ( fsInfo.f_bsize <= CDSTREAM_SECTOR_SIZE )
	{
		_gdwCdStreamFlags |= O_DIRECT;
		debug("Using no buffered loading for streaming\n");
	}
*/
	void *pBuffer = (void *)RwMallocAlign(CDSTREAM_SECTOR_SIZE, (RwUInt32)fsInfo.f_bsize);
	ASSERT( pBuffer != nil );

	gNumImages = 0;

	gNumChannels = numChannels;
	ASSERT( gNumChannels != 0 );

	gpReadInfo = (CdReadInfo *)calloc(numChannels, sizeof(CdReadInfo));
	ASSERT( gpReadInfo != nil );

	CDDEBUG("read info %p", gpReadInfo);

	CdStreamInitThread();

	ASSERT( pBuffer != nil );
	RwFreeAlign(pBuffer);
}

uint32
GetGTA3ImgSize(void)
{
	ASSERT( gImgFiles[0] > 0 );
	struct stat statbuf;

	char path[PATH_MAX];
	realpath(gImgNames[0], path);
	if (stat(path, &statbuf) == -1) {
		// Try case-insensitivity
		char* real = casepath(gImgNames[0], false);
		if (real)
		{
			realpath(real, path);
			free(real);
			if (stat(path, &statbuf) != -1)
				goto ok;
		}

		CDTRACE("can't get size of gta3.img");
		ASSERT(0);
		return 0;
	}
	ok:
	return (uint32)statbuf.st_size;
}

void
CdStreamShutdown(void)
{
    // Destroying semaphores and free(gpReadInfo) will be done at threads
#ifndef ONE_THREAD_PER_CHANNEL
	gCdStreamThreadStatus = 2;
	//sem_post(gCdStreamSema);
	pthread_mutex_unlock(&cdStreamMutex);
	pthread_join(_gCdStreamThread, nil);
#else
	for ( int32 i = 0; i < gNumChannels; i++ ) {
		gpReadInfo[i].nThreadStatus = 2;
		sem_post(gpReadInfo[i].pStartSemaphore);
		pthread_join(gpReadInfo[i].pChannelThread, nil);
	}
#endif
}


int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	ASSERT( channel < gNumChannels );
	ASSERT( buffer != nil );

	lastPosnRead = size + offset;

	ASSERT( _GET_INDEX(offset) < MAX_CDIMAGES );
	int32 hImage = gImgFiles[_GET_INDEX(offset)];
	ASSERT( hImage > 0 );

	CdReadInfo *pChannel = &gpReadInfo[channel];
	ASSERT( pChannel != nil );

	if ( pChannel->nSectorsToRead != 0 || pChannel->bReading ) {
		if (pChannel->hFile == hImage - 1 && pChannel->nSectorOffset == _GET_OFFSET(offset) && pChannel->nSectorsToRead >= size)
			return STREAM_SUCCESS;
#ifdef FLUSHABLE_STREAMING
		flushStream[channel] = 1;
		CdStreamSync(channel);
#else
		return STREAM_NONE;
#endif
	}

	pChannel->hFile = hImage - 1;
	pChannel->nStatus = STREAM_NONE;
	pChannel->nSectorOffset = _GET_OFFSET(offset);
	pChannel->nSectorsToRead = size;
	pChannel->pBuffer = buffer;
	pChannel->bLocked = 0;

#ifndef ONE_THREAD_PER_CHANNEL
	AddToQueue(&gChannelRequestQ, channel);
	if ( pthread_mutex_unlock(&cdStreamMutex) != 0 )
		printf("Signal Sema Error\n");
#else
	if ( sem_post(pChannel->pStartSemaphore) != 0 )
		printf("Signal Sema Error\n");
#endif

	return STREAM_SUCCESS;
}

int32
CdStreamGetStatus(int32 channel)
{
	ASSERT( channel < gNumChannels );
	CdReadInfo *pChannel = &gpReadInfo[channel];
	ASSERT( pChannel != nil );

#ifdef ONE_THREAD_PER_CHANNEL
	if (pChannel->nThreadStatus == 2)
		return STREAM_NONE;
#else
	if (gCdStreamThreadStatus == 2)
		return STREAM_NONE;
#endif

	if ( pChannel->bReading )
		return STREAM_READING;

	if ( pChannel->nSectorsToRead != 0 )
		return STREAM_WAITING;

	if ( pChannel->nStatus != STREAM_NONE )
	{
		int32 status = pChannel->nStatus;
		pChannel->nStatus = STREAM_NONE;

		return status;
	}

	return STREAM_NONE;
}

int32
CdStreamGetLastPosn(void)
{
	return lastPosnRead;
}

// wait for channel to finish reading
int32
CdStreamSync(int32 channel)
{
	ASSERT( channel < gNumChannels );
	CdReadInfo *pChannel = &gpReadInfo[channel];
	ASSERT( pChannel != nil );

#ifdef FLUSHABLE_STREAMING
	if (flushStream[channel]) {
		pChannel->nSectorsToRead = 0;
#ifdef ONE_THREAD_PER_CHANNEL
		pthread_kill(pChannel->pChannelThread, SIGUSR1);
		if (pChannel->bReading) {
			pChannel->bLocked = true;
#else
		if (pChannel->bReading) {
			pChannel->bLocked = true;
			pthread_kill(_gCdStreamThread, SIGUSR1);
#endif
			while (pChannel->bLocked)
				//sem_wait(pChannel->pDoneSemaphore);
				pthread_mutex_lock(&pChannel->doneMutex);
		}
		pChannel->bReading = false;
		flushStream[channel] = false;
		return STREAM_NONE;
	}
#endif

	if ( pChannel->nSectorsToRead != 0 )
	{
		pChannel->bLocked = true;
		while (pChannel->bLocked && pChannel->nSectorsToRead != 0){
			//sem_wait(pChannel->pDoneSemaphore);
			pthread_mutex_lock(&pChannel->doneMutex);
		}
		pChannel->bLocked = false;
	}

	pChannel->bReading = false;

	return pChannel->nStatus;
}

void
AddToQueue(Queue *queue, int32 item)
{
	ASSERT( queue != nil );
	ASSERT( queue->items != nil );
	queue->items[queue->tail] = item;

	queue->tail = (queue->tail + 1) % queue->size;

	if ( queue->head == queue->tail )
		debug("Queue is full\n");
}

int32
GetFirstInQueue(Queue *queue)
{
	ASSERT( queue != nil );
	if ( queue->head == queue->tail )
		return -1;

	ASSERT( queue->items != nil );
	return queue->items[queue->head];
}

void
RemoveFirstInQueue(Queue *queue)
{
	ASSERT( queue != nil );
	if ( queue->head == queue->tail )
	{
		debug("Queue is empty\n");
		return;
	}

	queue->head = (queue->head + 1) % queue->size;
}

void *CdStreamThread(void *param)
{
	debug("Created cdstream thread\n");

#ifndef ONE_THREAD_PER_CHANNEL
	while (gCdStreamThreadStatus != 2) {
		//sem_wait(gCdStreamSema);
		pthread_mutex_lock(&cdStreamMutex);

		int32 channel = GetFirstInQueue(&gChannelRequestQ);
		
		// spurious wakeup
		if (channel == -1)
			continue;
#else
	int channel = *((int*)param);
	while (gpReadInfo[channel].nThreadStatus != 2){
		sem_wait(gpReadInfo[channel].pStartSemaphore);
#endif

		CdReadInfo *pChannel = &gpReadInfo[channel];
		ASSERT( pChannel != nil );

		// spurious wakeup or we sent interrupt signal for flushing
		if(pChannel->nSectorsToRead == 0)
			continue;

		pChannel->bReading = true;

		// Not standard POSIX :shrug:
#ifdef __linux__
#ifdef ONE_THREAD_PER_CHANNEL
		if (gpReadInfo[channel].nThreadStatus == 0){
			gpReadInfo[channel].nThreadStatus = 1;
#else
		if (gCdStreamThreadStatus == 0){
			gCdStreamThreadStatus = 1;
#endif
			
			#ifdef SYS_gettid
			pid_t tid = syscall(SYS_gettid);
			int ret = setpriority(PRIO_PROCESS, tid, getpriority(PRIO_PROCESS, getpid()) + 1);
			#else
			#warning  "SYS_gettid unavailable on this system"
			#endif
		}
#endif
		if ( pChannel->nStatus == STREAM_NONE )
		{
			ASSERT(pChannel->hFile >= 0);
			ASSERT(pChannel->pBuffer != nil );

			lseek(pChannel->hFile, (size_t)pChannel->nSectorOffset * (size_t)CDSTREAM_SECTOR_SIZE, SEEK_SET);
			if (read(pChannel->hFile, pChannel->pBuffer, pChannel->nSectorsToRead * CDSTREAM_SECTOR_SIZE) == -1) {
				// pChannel->nSectorsToRead == 0 at this point means we wanted to flush channel
				// STREAM_WAITING is a little hack to make CStreaming not process this data
				pChannel->nStatus = pChannel->nSectorsToRead == 0 ? STREAM_WAITING : STREAM_ERROR;
			} else {
				pChannel->nStatus = STREAM_NONE;
			}
		}

#ifndef ONE_THREAD_PER_CHANNEL
		RemoveFirstInQueue(&gChannelRequestQ);
#endif

		pChannel->nSectorsToRead = 0;
		if ( pChannel->bLocked )
		{
			pChannel->bLocked = 0;
			//sem_post(pChannel->pDoneSemaphore);
			pthread_mutex_unlock(&pChannel->doneMutex);
		}
		pChannel->bReading = false;
	}
	char semName[20];
#ifndef ONE_THREAD_PER_CHANNEL
	for ( int32 i = 0; i < gNumChannels; i++ )
	{
		//RE3_SEM_CLOSE(gpReadInfo[i].pDoneSemaphore, "/semaphore_done%d", i);
		pthread_mutex_destroy(&gpReadInfo[i].doneMutex);
	}
	pthread_mutex_destroy(&cdStreamMutex);
	//RE3_SEM_CLOSE(gCdStreamSema, "/semaphore_cd_stream");
	free(gChannelRequestQ.items);
#else
	RE3_SEM_CLOSE(gpReadInfo[channel].pStartSemaphore, "/semaphore_start%d", channel);

	RE3_SEM_CLOSE(gpReadInfo[channel].pDoneSemaphore, "/semaphore_done%d", channel);
#endif
	if (gpReadInfo)
		free(gpReadInfo);
	gpReadInfo = nil;
	pthread_exit(nil);
}

bool
CdStreamAddImage(char const *path)
{
	ASSERT(path != nil);
	ASSERT(gNumImages < MAX_CDIMAGES);

	gImgFiles[gNumImages] = open(path, _gdwCdStreamFlags);
	debug("Opening %s\n",path);

	// Fix case sensitivity and backslashes.
	if (gImgFiles[gNumImages] == -1) {
		char* real = casepath(path, false);
		if (real)
		{
			gImgFiles[gNumImages] = open(real, _gdwCdStreamFlags);
			char cwd[MAX_PATH];
			getcwd(cwd, sizeof(cwd));
			debug("Image %d , cwd %s", gImgFiles[gNumImages], cwd);
			free(real);
		}
	}

	if ( gImgFiles[gNumImages] == -1 ) {
		assert(false);
		return false;
	}

	gImgNames[gNumImages] = strdup(path);
	gImgFiles[gNumImages]++; // because -1: error 0: not used

	strcpy(gCdImageNames[gNumImages], path);

	gNumImages++;

	return true;
}

char *
CdStreamGetImageName(int32 cd)
{
	ASSERT(cd < MAX_CDIMAGES);
	if ( gImgFiles[cd] > 0)
		return gCdImageNames[cd];

	return nil;
}

void
CdStreamRemoveImages(void)
{
	for ( int32 i = 0; i < gNumChannels; i++ ) {
#ifdef FLUSHABLE_STREAMING
		flushStream[i] = 1;
#endif
		CdStreamSync(i);
	}

	for ( int32 i = 0; i < gNumImages; i++ )
	{
		close(gImgFiles[i] - 1);
		free(gImgNames[i]);
		gImgFiles[i] = 0;
	}

	gNumImages = 0;
}

int32
CdStreamGetNumImages(void)
{
	return gNumImages;
}
#endif
