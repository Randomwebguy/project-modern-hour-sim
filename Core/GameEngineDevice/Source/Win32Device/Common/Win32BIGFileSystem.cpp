/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//////// Win32BIGFileSystem.h ///////////////////////////
// Bryan Cleveland, August 2002
/////////////////////////////////////////////////////////////

#include "Common/AudioAffect.h"
#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/file.h"
#include "Common/GameAudio.h"
#include "Common/GameMemory.h"
#include "Common/LocalFileSystem.h"

#if RTS_ZEROHOUR
#include "Common/Registry.h"
#endif

#include "Win32Device/Common/Win32BIGFile.h"
#include "Win32Device/Common/Win32BIGFileSystem.h"
#include "Utility/endian_compat.h"


static const char *BIGFileIdentifier = "BIGF";

#include "Common/ModelLoadDiagnostics.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

// Remaster M1 (15 Nov 2026): GENZH_GAME_DIR names the player's own Zero Hour install (the launcher finds it). The
// game's archives are read from there instead of shipping them with the remaster. Only the known Zero Hour and Generals
// archive names are taken (a player's mod or patch archives in that folder would desync LAN games), and they are loaded
// in the same order as when those files sat in the working directory, so nothing changes in which file wins.
static const char *GameDirFromEnvironment()
{
	const char *dir = std::getenv("GENZH_GAME_DIR");
	return (dir != nullptr && *dir != '\0') ? dir : nullptr;
}

static void AddKnownGameArchives(std::map<std::string, std::string> &archives)
{
	static const char *const Languages[] = { "English", "German", "French", "Spanish", "Italian", "Korean", "Chinese",
		"Brazilian", "Polish" };
	static const char *const ZeroHourFixed[] = { "AudioZH.big", "gensecZH.big", "INIZH.big", "MapsZH.big", "Music.big",
		"MusicZH.big", "PatchData.big", "PatchINI.big", "PatchWindow.big", "PatchZH.big", "ShadersZH.big", "SpeechZH.big",
		"TerrainZH.big", "TexturesZH.big", "W3DZH.big", "WindowZH.big" };
	static const char *const GeneralsFixed[] = { "Audio.big", "gensec.big", "INI.big", "maps.big", "Music.big", "Patch.big",
		"shaders.big", "Speech.big", "Terrain.big", "Textures.big", "W3D.big", "Window.big" };

	std::string root = GameDirFromEnvironment();
	if (!root.empty() && root.back() != '\\' && root.back() != '/')
		root += '\\';

	auto add = [&](const std::string &relative) {
		const std::string full = root + relative;
		const DWORD attributes = GetFileAttributesA(full.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) && archives.find(relative) == archives.end())
			archives[relative] = full;
	};
	for (const char *name : ZeroHourFixed)
		add(name);
	for (const char *name : GeneralsFixed)
		add(std::string("ZH_Generals\\") + name);
	for (const char *language : Languages) {
		const std::string l = language;
		add("Audio" + l + "ZH.big");
		add("Speech" + l + "ZH.big");
		add("W3D" + l + "ZH.big");
		add(l + "ZH.big");
		add("ZH_Generals\\Audio" + l + ".big");
		add("ZH_Generals\\Speech" + l + ".big");
		add("ZH_Generals\\" + l + ".big");
	}
}

Win32BIGFileSystem::Win32BIGFileSystem() : ArchiveFileSystem() {
}

Win32BIGFileSystem::~Win32BIGFileSystem() {
}

void Win32BIGFileSystem::init() {
	DEBUG_ASSERTCRASH(TheLocalFileSystem != nullptr, ("TheLocalFileSystem must be initialized before TheArchiveFileSystem."));
	if (TheLocalFileSystem == nullptr) {
		return;
	}

	if (GameDirFromEnvironment() != nullptr) {
		// Our own archives in the working directory and the player's game archives, merged and sorted by the name
		// relative to their folder: the order the working-directory scan produced when the game's files were copied in.
		FilenameList local;
		TheLocalFileSystem->getFileListInDirectory("", "", "*.big", local, TRUE);
		std::map<std::string, std::string> archives;
		for (FilenameListIter it = local.begin(); it != local.end(); ++it)
			archives[it->str()] = it->str();
		const size_t ownCount = archives.size();
		AddKnownGameArchives(archives);
		if (archives.find("INIZH.big") == archives.end() || archives.size() == ownCount) {
			fprintf(stderr, "ZHERROR gamedir: no Zero Hour archives (INIZH.big) in '%s'\n", GameDirFromEnvironment());
			fflush(stderr);
			exit(78);
		}
		FilenameList ordered;
		std::map<AsciiString, std::string> fullPaths;
		for (const auto &entry : archives) {
			ordered.insert(AsciiString(entry.first.c_str()));
			fullPaths[AsciiString(entry.first.c_str())] = entry.second;
		}
		for (FilenameListIter it = ordered.begin(); it != ordered.end(); ++it) {
			if (it->endsWithNoCase("Data\\INI\\INIZH.big") || it->endsWithNoCase("Data/INI/INIZH.big"))
				continue;
			const std::string &full = fullPaths[*it];
			ArchiveFile *archiveFile = openArchiveFile(full.c_str());
			if (archiveFile != nullptr) {
				RecordFileOpen(full.c_str(), "mount");
				loadIntoDirectoryTree(archiveFile, FALSE);
				m_archiveFileMap[AsciiString(full.c_str())] = archiveFile;
			}
		}
		Bool hasGeneralsBase = FALSE;
		for (const auto &entry : archives)
			if (entry.first.rfind("ZH_Generals\\", 0) == 0)
				hasGeneralsBase = TRUE;
		if (hasGeneralsBase)
			return;
		// An install without a bundled ZH_Generals folder (retail / EA app): the Generals base comes from its own
		// install, as before.
	} else {
		loadBigFilesFromDirectory("", "*.big");
	}

#if RTS_ZEROHOUR
    // load original Generals assets
    AsciiString installPath;
    GetStringFromGeneralsRegistry("", "InstallPath", installPath );
	// TheSuperHackers @feature Use the bundled Generals assets in Steam installations
	// when the legacy registry path is absent or points to an unavailable directory.
	DWORD installAttributes = installPath.isEmpty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesA(installPath.str());
	if (installAttributes == INVALID_FILE_ATTRIBUTES || !(installAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		DWORD bundledAttributes = GetFileAttributesA("ZH_Generals");
		if (bundledAttributes != INVALID_FILE_ATTRIBUTES && (bundledAttributes & FILE_ATTRIBUTE_DIRECTORY))
			installPath = "ZH_Generals\\";
	}
    //@todo this will need to be ramped up to a crash for release
    DEBUG_ASSERTCRASH(!installPath.isEmpty(), ("Be 1337! Go install Generals!"));
    if (!installPath.isEmpty())
      loadBigFilesFromDirectory(installPath, "*.big");
#endif
}

void Win32BIGFileSystem::reset() {
}

void Win32BIGFileSystem::update() {
}

void Win32BIGFileSystem::postProcessLoad() {
}

ArchiveFile * Win32BIGFileSystem::openArchiveFile(const Char *filename) {
	File *fp = TheLocalFileSystem->openFile(filename, File::READ | File::BINARY);
	AsciiString archiveFileName;
	archiveFileName = filename;
	archiveFileName.toLower();
	Int archiveFileSize = 0;
	Int numLittleFiles = 0;

	DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - opening BIG file %s", filename));

	if (fp == nullptr) {
		DEBUG_CRASH(("Could not open archive file %s for parsing", filename));
		return nullptr;
	}

	AsciiString asciibuf;
	char buffer[_MAX_PATH];
	fp->read(buffer, 4); // read the "BIG" at the beginning of the file.
	buffer[4] = 0;
	if (strcmp(buffer, BIGFileIdentifier) != 0) {
		DEBUG_CRASH(("Error reading BIG file identifier in file %s", filename));
		fp->close();
		fp = nullptr;
		return nullptr;
	}

	// read in the file size.
	fp->read(&archiveFileSize, 4);

	DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - size of archive file is %d bytes", archiveFileSize));

//	char t;

	// read in the number of files contained in this BIG file.
	// change the order of the bytes cause the file size is in reverse byte order for some reason.
	fp->read(&numLittleFiles, 4);
	numLittleFiles = betoh(numLittleFiles);

	DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - %d are contained in archive", numLittleFiles));
//	for (Int i = 0; i < 2; ++i) {
//		t = buffer[i];
//		buffer[i] = buffer[(4-i)-1];
//		buffer[(4-i)-1] = t;
//	}

	// seek to the beginning of the directory listing.
	fp->seek(0x10, File::START);
	// read in each directory listing.
	ArchivedFileInfo fileInfo;
	ArchiveFile *archiveFile = NEW Win32BIGFile(filename, AsciiString::TheEmptyString);

	for (Int i = 0; i < numLittleFiles; ++i) {
		Int filesize = 0;
		Int fileOffset = 0;
		fp->read(&fileOffset, 4);
		fp->read(&filesize, 4);

		filesize = betoh(filesize);
		fileOffset = betoh(fileOffset);

		fileInfo.m_archiveFilename = archiveFileName;
		fileInfo.m_offset = fileOffset;
		fileInfo.m_size = filesize;

		// read in the path name of the file.
		Int pathIndex = -1;
		do {
			++pathIndex;
			fp->read(buffer + pathIndex, 1);
		} while (buffer[pathIndex] != 0);

		Int filenameIndex = pathIndex;
		while ((filenameIndex >= 0) && (buffer[filenameIndex] != '\\') && (buffer[filenameIndex] != '/')) {
			--filenameIndex;
		}

		fileInfo.m_filename = (char *)(buffer + filenameIndex + 1);
		fileInfo.m_filename.toLower();
		buffer[filenameIndex + 1] = 0;

		AsciiString path;
		path = buffer;

		AsciiString debugpath;
		debugpath = path;
		debugpath.concat(fileInfo.m_filename);
//		DEBUG_LOG(("Win32BIGFileSystem::openArchiveFile - adding file %s to archive file %s, file number %d", debugpath.str(), fileInfo->m_archiveFilename.str(), i));

		archiveFile->addFile(path, &fileInfo);
	}

	archiveFile->attachFile(fp);

	// leave fp open as the archive file will be using it.

	return archiveFile;
}

void Win32BIGFileSystem::closeArchiveFile(const Char *filename) {
	// Need to close the specified big file
	ArchiveFileMap::iterator it =  m_archiveFileMap.find(filename);
	if (it == m_archiveFileMap.end()) {
		return;
	}

	if (stricmp(filename, MUSIC_BIG) == 0) {
		// Stop the current audio
		TheAudio->stopAudio(AudioAffect_Music);

		// No need to turn off other audio, as the lookups will just fail.
	}
	DEBUG_ASSERTCRASH(stricmp(filename, MUSIC_BIG) == 0, ("Attempting to close Archive file '%s', need to add code to handle its shutdown correctly.", filename));

	// may need to do some other processing here first.

	delete (it->second);
	m_archiveFileMap.erase(it);
}

void Win32BIGFileSystem::closeAllArchiveFiles() {
}

void Win32BIGFileSystem::closeAllFiles() {
}

Bool Win32BIGFileSystem::loadBigFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite) {

	FilenameList filenameList;
	TheLocalFileSystem->getFileListInDirectory(dir, "", fileMask, filenameList, TRUE);

	Bool actuallyAdded = FALSE;
	FilenameListIter it = filenameList.begin();
	while (it != filenameList.end()) {
#if RTS_ZEROHOUR
		// TheSuperHackers @bugfix bobtista 18/11/2025 Skip duplicate INIZH.big in Data\INI to prevent CRC mismatches.
		// English, Chinese, and Korean SKUs shipped with two INIZH.big files (one in Run directory, one in Run\Data\INI).
		// The DeleteFile cleanup doesn't work on EA App/Origin installs because the folder is not writable, so we skip loading it instead.
		if (it->endsWithNoCase("Data\\INI\\INIZH.big") || it->endsWithNoCase("Data/INI/INIZH.big")) {
			it++;
			continue;
		}
#endif

		ArchiveFile *archiveFile = openArchiveFile((*it).str());

		if (archiveFile != nullptr) {
			DEBUG_LOG(("Win32BIGFileSystem::loadBigFilesFromDirectory - loading %s into the directory tree.", (*it).str()));
			RecordFileOpen((*it).str(), "mount");
			loadIntoDirectoryTree(archiveFile, overwrite);
			m_archiveFileMap[(*it)] = archiveFile;
			DEBUG_LOG(("Win32BIGFileSystem::loadBigFilesFromDirectory - %s inserted into the archive file map.", (*it).str()));
			actuallyAdded = TRUE;
		}

		it++;
	}

	return actuallyAdded;
}
