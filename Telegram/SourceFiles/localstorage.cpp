/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"
#include "localstorage.h"

#include "lang.h"

namespace {
	enum StickerSetType {
		StickerSetTypeEmpty     = 0,
		StickerSetTypeID        = 1,
		StickerSetTypeShortName = 2,
	};

	typedef quint64 FileKey;

	static const char tdfMagic[] = { 'T', 'D', 'F', '$' };
	static const int32 tdfMagicLen = sizeof(tdfMagic);

	QString toFilePart(FileKey val) {
		QString result;
		result.reserve(0x10);
		for (int32 i = 0; i < 0x10; ++i) {
			uchar v = (val & 0x0F);
			result.push_back((v < 0x0A) ? ('0' + v) : ('A' + (v - 0x0A)));
			val >>= 4;
		}
		return result;
	}

	FileKey fromFilePart(const QString &val) {
		FileKey result = 0;
		int32 i = val.size();
		if (i != 0x10) return 0;

		while (i > 0) {
			--i;
			result <<= 4;

			uint16 ch = val.at(i).unicode();
			if (ch >= 'A' && ch <= 'F') {
				result |= (ch - 'A') + 0x0A;
			} else if (ch >= '0' && ch <= '9') {
				result |= (ch - '0');
			} else {
				return 0;
			}
		}
		return result;
	}

	QString _basePath, _userBasePath;

	bool _started = false;
	_local_inner::Manager *_manager = 0;

	bool _working() {
		return _manager && !_basePath.isEmpty();
	}

	bool _userWorking() {
		return _manager && !_basePath.isEmpty() && !_userBasePath.isEmpty();
	}

	enum FileOptions {
		UserPath = 0x01,
		SafePath = 0x02,
	};

	bool keyAlreadyUsed(QString &name, int options = UserPath | SafePath) {
		name += '0';
		if (QFileInfo(name).exists()) return true;
		if (options & SafePath) {
			name[name.size() - 1] = '1';
			return QFileInfo(name).exists();
		}
		return false;
	}

	FileKey genKey(int options = UserPath | SafePath) {
		if (options & UserPath) {
			if (!_userWorking()) return 0;
		} else {
			if (!_working()) return 0;
		}

		FileKey result;
		QString base = (options & UserPath) ? _userBasePath : _basePath, path;
		path.reserve(base.size() + 0x11);
		path += base;
		do {
			result = MTP::nonce<FileKey>();
			path.resize(base.size());
			path += toFilePart(result);
		} while (!result || keyAlreadyUsed(path, options));

		return result;
	}

	void clearKey(const FileKey &key, int options = UserPath | SafePath) {
		if (options & UserPath) {
			if (!_userWorking()) return;
		} else {
			if (!_working()) return;
		}

		QString base = (options & UserPath) ? _userBasePath : _basePath, name;
		name.reserve(base.size() + 0x11);
		name.append(base).append(toFilePart(key)).append('0');
		QFile::remove(name);
		if (options & SafePath) {
			name[name.size() - 1] = '1';
			QFile::remove(name);
		}
	}

	bool _checkStreamStatus(QDataStream &stream) {
		if (stream.status() != QDataStream::Ok) {
			LOG(("Bad data stream status: %1").arg(stream.status()));
			return false;
		}
		return true;
	}

	uint32 _dateTimeSize() {
		return (sizeof(qint64) + sizeof(quint32) + sizeof(qint8));
	}

	uint32 _stringSize(const QString &str) {
		return sizeof(quint32) + str.size() * sizeof(ushort);
	}

	uint32 _bytearraySize(const QByteArray &arr) {
		return sizeof(quint32) + arr.size();
	}

	QByteArray _settingsSalt, _passKeySalt, _passKeyEncrypted;

	mtpAuthKey _oldKey, _settingsKey, _passKey, _localKey;
	void createLocalKey(const QByteArray &pass, QByteArray *salt, mtpAuthKey *result) {
		uchar key[LocalEncryptKeySize] = { 0 };
		int32 iterCount = pass.size() ? LocalEncryptIterCount : LocalEncryptNoPwdIterCount; // dont slow down for no password
		QByteArray newSalt;
		if (!salt) {
			newSalt.resize(LocalEncryptSaltSize);
			memset_rand(newSalt.data(), newSalt.size());
			salt = &newSalt;

			cSetLocalSalt(newSalt);
		}

		PKCS5_PBKDF2_HMAC_SHA1(pass.constData(), pass.size(), (uchar*)salt->data(), salt->size(), iterCount, LocalEncryptKeySize, key);

		result->setKey(key);
	}

	struct FileReadDescriptor {
		FileReadDescriptor() : version(0) {
		}
		int32 version;
		QByteArray data;
		QBuffer buffer;
		QDataStream stream;
		~FileReadDescriptor() {
			if (version) {
				stream.setDevice(0);
				if (buffer.isOpen()) buffer.close();
				buffer.setBuffer(0);
			}
		}
	};

	struct EncryptedDescriptor {
		EncryptedDescriptor() {
		}
		EncryptedDescriptor(uint32 size) {
			uint32 fullSize = sizeof(uint32) + size;
			if (fullSize & 0x0F) fullSize += 0x10 - (fullSize & 0x0F);
			data.reserve(fullSize);

			data.resize(sizeof(uint32));
			buffer.setBuffer(&data);
			buffer.open(QIODevice::WriteOnly);
			buffer.seek(sizeof(uint32));
			stream.setDevice(&buffer);
			stream.setVersion(QDataStream::Qt_5_1);
		}
		QByteArray data;
		QBuffer buffer;
		QDataStream stream;
		void finish() {
			if (stream.device()) stream.setDevice(0);
			if (buffer.isOpen()) buffer.close();
			buffer.setBuffer(0);
		}
		~EncryptedDescriptor() {
			finish();
		}
	};

	struct FileWriteDescriptor {
		FileWriteDescriptor(const FileKey &key, int options = UserPath | SafePath) : dataSize(0) {
			init(toFilePart(key), options);
		}
		FileWriteDescriptor(const QString &name, int options = UserPath | SafePath) : dataSize(0) {
			init(name, options);
		}
		void init(const QString &name, int options) {
			if (options & UserPath) {
				if (!_userWorking()) return;
			} else {
				if (!_working()) return;
			}

			// detect order of read attempts and file version
			QString toTry[2];
			toTry[0] = ((options & UserPath) ? _userBasePath : _basePath) + name + '0';
			if (options & SafePath) {
				toTry[1] = ((options & UserPath) ? _userBasePath : _basePath) + name + '1';
				QFileInfo toTry0(toTry[0]);
				QFileInfo toTry1(toTry[1]);
				if (toTry0.exists()) {
					if (toTry1.exists()) {
						QDateTime mod0 = toTry0.lastModified(), mod1 = toTry1.lastModified();
						if (mod0 > mod1) {
							qSwap(toTry[0], toTry[1]);
						}
					} else {
						qSwap(toTry[0], toTry[1]);
					}
					toDelete = toTry[1];
				} else if (toTry1.exists()) {
					toDelete = toTry[1];
				}
			}

			file.setFileName(toTry[0]);
			if (file.open(QIODevice::WriteOnly)) {
				file.write(tdfMagic, tdfMagicLen);
				qint32 version = AppVersion;
				file.write((const char*)&version, sizeof(version));

				stream.setDevice(&file);
				stream.setVersion(QDataStream::Qt_5_1);
			}
		}
		bool writeData(const QByteArray &data) {
			if (!file.isOpen()) return false;

			stream << data;
			quint32 len = data.isNull() ? 0xffffffff : data.size();
			if (QSysInfo::ByteOrder != QSysInfo::BigEndian) {
				len = qbswap(len);
			}
			md5.feed(&len, sizeof(len));
			md5.feed(data.constData(), data.size());
			dataSize += sizeof(len) + data.size();

			return true;
		}
		static QByteArray prepareEncrypted(EncryptedDescriptor &data, const mtpAuthKey &key = _localKey) {
			data.finish();
			QByteArray &toEncrypt(data.data);

			// prepare for encryption
			uint32 size = toEncrypt.size(), fullSize = size;
			if (fullSize & 0x0F) {
				fullSize += 0x10 - (fullSize & 0x0F);
				toEncrypt.resize(fullSize);
				memset_rand(toEncrypt.data() + size, fullSize - size);
			}
			*(uint32*)toEncrypt.data() = size;
			QByteArray encrypted(0x10 + fullSize, Qt::Uninitialized); // 128bit of sha1 - key128, sizeof(data), data
			hashSha1(toEncrypt.constData(), toEncrypt.size(), encrypted.data());
			aesEncryptLocal(toEncrypt.constData(), encrypted.data() + 0x10, fullSize, &key, encrypted.constData());

			return encrypted;
		}
		bool writeEncrypted(EncryptedDescriptor &data, const mtpAuthKey &key = _localKey) {
			return writeData(prepareEncrypted(data, key));
		}
		void finish() {
			if (!file.isOpen()) return;

			stream.setDevice(0);

			md5.feed(&dataSize, sizeof(dataSize));
			qint32 version = AppVersion;
			md5.feed(&version, sizeof(version));
			md5.feed(tdfMagic, tdfMagicLen);
			file.write((const char*)md5.result(), 0x10);
			file.close();

			if (!toDelete.isEmpty()) {
				QFile::remove(toDelete);
			}
		}
		QFile file;
		QDataStream stream;

		QString toDelete;

		HashMd5 md5;
		int32 dataSize;

		~FileWriteDescriptor() {
			finish();
		}
	};

	bool readFile(FileReadDescriptor &result, const QString &name, int options = UserPath | SafePath) {
		if (options & UserPath) {
			if (!_userWorking()) return false;
		} else {
			if (!_working()) return false;
		}

		// detect order of read attempts
		QString toTry[2];
		toTry[0] = ((options & UserPath) ? _userBasePath : _basePath) + name + '0';
		if (options & SafePath) {
			QFileInfo toTry0(toTry[0]);
			if (toTry0.exists()) {
				toTry[1] = ((options & UserPath) ? _userBasePath : _basePath) + name + '1';
				QFileInfo toTry1(toTry[1]);
				if (toTry1.exists()) {
					QDateTime mod0 = toTry0.lastModified(), mod1 = toTry1.lastModified();
					if (mod0 < mod1) {
						qSwap(toTry[0], toTry[1]);
					}
				} else {
					toTry[1] = QString();
				}
			} else {
				toTry[0][toTry[0].size() - 1] = '1';
			}
		}
		for (int32 i = 0; i < 2; ++i) {
			QString fname(toTry[i]);
			if (fname.isEmpty()) break;

			QFile f(fname);
			if (!f.open(QIODevice::ReadOnly)) {
				DEBUG_LOG(("App Info: failed to open '%1' for reading").arg(name));
				continue;
			}

			// check magic
			char magic[tdfMagicLen];
			if (f.read(magic, tdfMagicLen) != tdfMagicLen) {
				DEBUG_LOG(("App Info: failed to read magic from '%1'").arg(name));
				continue;
			}
			if (memcmp(magic, tdfMagic, tdfMagicLen)) {
				DEBUG_LOG(("App Info: bad magic %1 in '%2'").arg(mb(magic, tdfMagicLen).str()).arg(name));
				continue;
			}

			// read app version
			qint32 version;
			if (f.read((char*)&version, sizeof(version)) != sizeof(version)) {
				DEBUG_LOG(("App Info: failed to read version from '%1'").arg(name));
				continue;
			}
			if (version > AppVersion) {
				DEBUG_LOG(("App Info: version too big %1 for '%2', my version %3").arg(version).arg(name).arg(AppVersion));
				continue;
			}

			// read data
			QByteArray bytes = f.read(f.size());
			int32 dataSize = bytes.size() - 16;
			if (dataSize < 0) {
				DEBUG_LOG(("App Info: bad file '%1', could not read sign part").arg(name));
				continue;
			}

			// check signature
			HashMd5 md5;
			md5.feed(bytes.constData(), dataSize);
			md5.feed(&dataSize, sizeof(dataSize));
			md5.feed(&version, sizeof(version));
			md5.feed(magic, tdfMagicLen);
			if (memcmp(md5.result(), bytes.constData() + dataSize, 16)) {
				DEBUG_LOG(("App Info: bad file '%1', signature did not match").arg(name));
				continue;
			}

			bytes.resize(dataSize);
			result.data = bytes;
			bytes = QByteArray();

			result.version = version;
			result.buffer.setBuffer(&result.data);
			result.buffer.open(QIODevice::ReadOnly);
			result.stream.setDevice(&result.buffer);
			result.stream.setVersion(QDataStream::Qt_5_1);

			if ((i == 0 && !toTry[1].isEmpty()) || i == 1) {
				QFile::remove(toTry[1 - i]);
			}

			return true;
		}
		return false;
	}

	bool decryptLocal(EncryptedDescriptor &result, const QByteArray &encrypted, const mtpAuthKey &key = _localKey) {
		if (encrypted.size() <= 16 || (encrypted.size() & 0x0F)) {
			LOG(("App Error: bad encrypted part size: %1").arg(encrypted.size()));
			return false;
		}
		uint32 fullLen = encrypted.size() - 16;

		QByteArray decrypted;
		decrypted.resize(fullLen);
		const char *encryptedKey = encrypted.constData(), *encryptedData = encrypted.constData() + 16;
		aesDecryptLocal(encryptedData, decrypted.data(), fullLen, &key, encryptedKey);
		uchar sha1Buffer[20];
		if (memcmp(hashSha1(decrypted.constData(), decrypted.size(), sha1Buffer), encryptedKey, 16)) {
			LOG(("App Info: bad decrypt key, data not decrypted - incorrect password?"));
			return false;
		}

		uint32 dataLen = *(const uint32*)decrypted.constData();
		if (dataLen > uint32(decrypted.size()) || dataLen <= fullLen - 16 || dataLen < sizeof(uint32)) {
			LOG(("App Error: bad decrypted part size: %1, fullLen: %2, decrypted size: %3").arg(dataLen).arg(fullLen).arg(decrypted.size()));
			return false;
		}

		decrypted.resize(dataLen);
		result.data = decrypted;
		decrypted = QByteArray();

		result.buffer.setBuffer(&result.data);
		result.buffer.open(QIODevice::ReadOnly);
		result.buffer.seek(sizeof(uint32)); // skip len
		result.stream.setDevice(&result.buffer);
		result.stream.setVersion(QDataStream::Qt_5_1);

		return true;
	}

	bool readEncryptedFile(FileReadDescriptor &result, const QString &name, int options = UserPath | SafePath, const mtpAuthKey &key = _localKey) {
		if (!readFile(result, name, options)) {
			return false;
		}
		QByteArray encrypted;
		result.stream >> encrypted;

		EncryptedDescriptor data;
		if (!decryptLocal(data, encrypted, key)) {
			result.stream.setDevice(0);
			if (result.buffer.isOpen()) result.buffer.close();
			result.buffer.setBuffer(0);
			result.data = QByteArray();
			result.version = 0;
			return false;
		}

		result.stream.setDevice(0);
		if (result.buffer.isOpen()) result.buffer.close();
		result.buffer.setBuffer(0);
		result.data = data.data;
		result.buffer.setBuffer(&result.data);
		result.buffer.open(QIODevice::ReadOnly);
		result.buffer.seek(data.buffer.pos());
		result.stream.setDevice(&result.buffer);
		result.stream.setVersion(QDataStream::Qt_5_1);

		return true;
	}

	bool readEncryptedFile(FileReadDescriptor &result, const FileKey &fkey, int options = UserPath | SafePath, const mtpAuthKey &key = _localKey) {
		return readEncryptedFile(result, toFilePart(fkey), options, key);
	}

	FileKey _dataNameKey = 0;

	enum { // Local Storage Keys
		lskUserMap           = 0x00,
		lskDraft             = 0x01, // data: PeerId peer
		lskDraftPosition     = 0x02, // data: PeerId peer
		lskImages            = 0x03, // data: StorageKey location
		lskLocations         = 0x04, // no data
		lskStickerImages     = 0x05, // data: StorageKey location
		lskAudios            = 0x06, // data: StorageKey location
		lskRecentStickersOld = 0x07, // no data
		lskBackground        = 0x08, // no data
		lskUserSettings      = 0x09, // no data
		lskRecentHashtags    = 0x0a, // no data
		lskStickers          = 0x0b, // no data
	};

	typedef QMap<PeerId, FileKey> DraftsMap;
	DraftsMap _draftsMap, _draftsPositionsMap;
	typedef QMap<PeerId, bool> DraftsNotReadMap;
	DraftsNotReadMap _draftsNotReadMap;

	typedef QMultiMap<MediaKey, FileLocation> FileLocations;
	FileLocations _fileLocations;
	typedef QPair<MediaKey, FileLocation> FileLocationPair;
	typedef QMap<QString, FileLocationPair> FileLocationPairs;
	FileLocationPairs _fileLocationPairs;
	FileKey _locationsKey = 0;
	
	FileKey _recentStickersKeyOld = 0, _stickersKey = 0;
	
	FileKey _backgroundKey = 0;
	bool _backgroundWasRead = false;

	FileKey _userSettingsKey = 0;
	FileKey _recentHashtagsKey = 0;
	bool _recentHashtagsWereRead = false;

	typedef QPair<FileKey, qint32> FileDesc; // file, size
	typedef QMap<StorageKey, FileDesc> StorageMap;
	StorageMap _imagesMap, _stickerImagesMap, _audiosMap;
	int32 _storageImagesSize = 0, _storageStickersSize = 0, _storageAudiosSize = 0;

	bool _mapChanged = false;
	int32 _oldMapVersion = 0;

	enum WriteMapWhen {
		WriteMapNow,
		WriteMapFast,
		WriteMapSoon,
	};

	void _writeMap(WriteMapWhen when = WriteMapSoon);
		
	void _writeLocations(WriteMapWhen when = WriteMapSoon) {
		if (when != WriteMapNow) {
			_manager->writeLocations(when == WriteMapFast);
			return;
		}
		if (!_working()) return;

		_manager->writingLocations();
		if (_fileLocations.isEmpty()) {
			if (_locationsKey) {
				clearKey(_locationsKey);
				_locationsKey = 0;
				_mapChanged = true;
				_writeMap();
			}
		} else {
			if (!_locationsKey) {
				_locationsKey = genKey();
				_mapChanged = true;
				_writeMap(WriteMapFast);
			}
			quint32 size = 0;
			for (FileLocations::const_iterator i = _fileLocations.cbegin(); i != _fileLocations.cend(); ++i) {
				// location + type + namelen + name + date + size
				size += sizeof(quint64) * 2 + sizeof(quint32) + _stringSize(i.value().name) + _dateTimeSize() + sizeof(quint32);
			}
			EncryptedDescriptor data(size);
			for (FileLocations::const_iterator i = _fileLocations.cbegin(); i != _fileLocations.cend(); ++i) {
				data.stream << quint64(i.key().first) << quint64(i.key().second) << quint32(i.value().type) << i.value().name << i.value().modified << quint32(i.value().size);
			}
			FileWriteDescriptor file(_locationsKey);
			file.writeEncrypted(data);
		}
	}

	void _readLocations() {
		FileReadDescriptor locations;
		if (!readEncryptedFile(locations, _locationsKey)) {
			clearKey(_locationsKey);
			_locationsKey = 0;
			_writeMap();
			return;
		}

		while (!locations.stream.atEnd()) {
			quint64 first, second;
			FileLocation loc;
			quint32 type;
			locations.stream >> first >> second >> type >> loc.name >> loc.modified >> loc.size;

			MediaKey key(first, second);
			loc.type = StorageFileType(type);

			if (loc.check()) {
				_fileLocations.insert(key, loc);
				_fileLocationPairs.insert(loc.name, FileLocationPair(key, loc));
			} else {
				_writeLocations();
			}
		}
	}

	mtpDcOptions *_dcOpts = 0;
	bool _readSetting(quint32 blockId, QDataStream &stream, int version) {
		switch (blockId) {
		case dbiDcOption: {
			quint32 dcId, port;
			QString host, ip;
			stream >> dcId >> host >> ip >> port;
			if (!_checkStreamStatus(stream)) return false;

			if (_dcOpts) _dcOpts->insert(dcId, mtpDcOption(dcId, host.toUtf8().constData(), ip.toUtf8().constData(), port));
		} break;

		case dbiMaxGroupCount: {
			qint32 maxSize;
			stream >> maxSize;
			if (!_checkStreamStatus(stream)) return false;

			cSetMaxGroupCount(maxSize);
		} break;

		case dbiUser: {
			quint32 dcId;
			qint32 uid;
			stream >> uid >> dcId;
			if (!_checkStreamStatus(stream)) return false;

			DEBUG_LOG(("MTP Info: user found, dc %1, uid %2").arg(dcId).arg(uid));
			MTP::configure(dcId, uid);
		} break;

		case dbiKey: {
			qint32 dcId;
			quint32 key[64];
			stream >> dcId;
			stream.readRawData((char*)key, 256);
			if (!_checkStreamStatus(stream)) return false;

			DEBUG_LOG(("MTP Info: key found, dc %1, key: %2").arg(dcId).arg(mb(key, 256).str()));
			dcId = dcId % _mtp_internal::dcShift;
			mtpAuthKeyPtr keyPtr(new mtpAuthKey());
			keyPtr->setKey(key);
			keyPtr->setDC(dcId);

			MTP::setKey(dcId, keyPtr);
		} break;

		case dbiAutoStart: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetAutoStart(v == 1);
		} break;

		case dbiStartMinimized: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetStartMinimized(v == 1);
		} break;

		case dbiSendToMenu: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetSendToMenu(v == 1);
		} break;

		case dbiSoundNotify: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetSoundNotify(v == 1);
		} break;

		case dbiDesktopNotify: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetDesktopNotify(v == 1);
		} break;

		case dbiWorkMode: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			switch (v) {
			case dbiwmTrayOnly: cSetWorkMode(dbiwmTrayOnly); break;
			case dbiwmWindowOnly: cSetWorkMode(dbiwmWindowOnly); break;
			default: cSetWorkMode(dbiwmWindowAndTray); break;
			};
		} break;

		case dbiConnectionType: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			switch (v) {
			case dbictHttpProxy:
			case dbictTcpProxy: {
				ConnectionProxy p;
				qint32 port;
				stream >> p.host >> port >> p.user >> p.password;
				if (!_checkStreamStatus(stream)) return false;

				p.port = uint32(port);
				cSetConnectionProxy(p);
			}
				cSetConnectionType(DBIConnectionType(v));
				break;
			case dbictHttpAuto:
			default: cSetConnectionType(dbictAuto); break;
			};
		} break;

		case dbiSeenTrayTooltip: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetSeenTrayTooltip(v == 1);
		} break;

		case dbiAutoUpdate: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetAutoUpdate(v == 1);
		} break;

		case dbiLastUpdateCheck: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetLastUpdateCheck(v);
		} break;

		case dbiScale: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			DBIScale s = cRealScale();
			switch (v) {
			case dbisAuto: s = dbisAuto; break;
			case dbisOne: s = dbisOne; break;
			case dbisOneAndQuarter: s = dbisOneAndQuarter; break;
			case dbisOneAndHalf: s = dbisOneAndHalf; break;
			case dbisTwo: s = dbisTwo; break;
			}
			if (cRetina()) s = dbisOne;
			cSetConfigScale(s);
			cSetRealScale(s);
		} break;

		case dbiLang: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			if (v == languageTest || (v >= 0 && v < languageCount)) {
				cSetLang(v);
			}
		} break;

		case dbiLangFile: {
			QString v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetLangFile(v);
		} break;

		case dbiWindowPosition: {
			TWindowPos pos;
			stream >> pos.x >> pos.y >> pos.w >> pos.h >> pos.moncrc >> pos.maximized;
			if (!_checkStreamStatus(stream)) return false;

			cSetWindowPos(pos);
		} break;

		case dbiLoggedPhoneNumber: {
			QString v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetLoggedPhoneNumber(v);
		} break;

		case dbiMutePeer: { // deprecated
			quint64 peerId;
			stream >> peerId;
			if (!_checkStreamStatus(stream)) return false;
		} break;

		case dbiMutedPeers: { // deprecated
			quint32 count;
			stream >> count;
			if (!_checkStreamStatus(stream)) return false;

			for (uint32 i = 0; i < count; ++i) {
				quint64 peerId;
				stream >> peerId;
			}
			if (!_checkStreamStatus(stream)) return false;
		} break;

		case dbiSendKey: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetCtrlEnter(v == dbiskCtrlEnter);
		} break;

		case dbiCatsAndDogs: { // deprecated
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;
		} break;

		case dbiTileBackground: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetTileBackground(v == 1);
			if (version < 8005 && !_backgroundKey) {
				cSetTileBackground(false);
			}
		} break;

		case dbiAutoLock: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetAutoLock(v);
		} break;

		case dbiReplaceEmojis: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetReplaceEmojis(v == 1);
		} break;

		case dbiDefaultAttach: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			switch (v) {
			case dbidaPhoto: cSetDefaultAttach(dbidaPhoto); break;
			default: cSetDefaultAttach(dbidaDocument); break;
			}
		} break;

		case dbiNotifyView: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			switch (v) {
			case dbinvShowNothing: cSetNotifyView(dbinvShowNothing); break;
			case dbinvShowName: cSetNotifyView(dbinvShowName); break;
			default: cSetNotifyView(dbinvShowPreview); break;
			}
		} break;

		case dbiAskDownloadPath: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetAskDownloadPath(v == 1);
		} break;

		case dbiDownloadPath: {
			QString v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetDownloadPath(v);
		} break;

		case dbiCompressPastedImage: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetCompressPastedImage(v == 1);
		} break;

		case dbiEmojiTab: {
			qint32 v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			switch (v) {
			case dbietRecent     : cSetEmojiTab(dbietRecent     ); break;
			case dbietPeople     : cSetEmojiTab(dbietPeople     ); break;
			case dbietNature     : cSetEmojiTab(dbietNature     ); break;
			case dbietFood       : cSetEmojiTab(dbietFood       ); break;
			case dbietCelebration: cSetEmojiTab(dbietCelebration); break;
			case dbietActivity   : cSetEmojiTab(dbietActivity   ); break;
			case dbietTravel     : cSetEmojiTab(dbietTravel     ); break;
			case dbietObjects    : cSetEmojiTab(dbietObjects    ); break;
			case dbietStickers   : cSetEmojiTab(dbietStickers   ); break;
			}
		} break;

		case dbiRecentEmojisOld: {
			RecentEmojisPreloadOld v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			if (!v.isEmpty()) {
				RecentEmojisPreload p;
				p.reserve(v.size());
				for (int i = 0; i < v.size(); ++i) {
					uint64 e(v.at(i).first);
					switch (e) {
						case 0xD83CDDEFLLU: e = 0xD83CDDEFD83CDDF5LLU; break;
						case 0xD83CDDF0LLU: e = 0xD83CDDF0D83CDDF7LLU; break;
						case 0xD83CDDE9LLU: e = 0xD83CDDE9D83CDDEALLU; break;
						case 0xD83CDDE8LLU: e = 0xD83CDDE8D83CDDF3LLU; break;
						case 0xD83CDDFALLU: e = 0xD83CDDFAD83CDDF8LLU; break;
						case 0xD83CDDEBLLU: e = 0xD83CDDEBD83CDDF7LLU; break;
						case 0xD83CDDEALLU: e = 0xD83CDDEAD83CDDF8LLU; break;
						case 0xD83CDDEELLU: e = 0xD83CDDEED83CDDF9LLU; break;
						case 0xD83CDDF7LLU: e = 0xD83CDDF7D83CDDFALLU; break;
						case 0xD83CDDECLLU: e = 0xD83CDDECD83CDDE7LLU; break;
					}
					p.push_back(qMakePair(e, v.at(i).second));
				}
				cSetRecentEmojisPreload(p);
			}
		} break;

		case dbiRecentEmojis: {
			RecentEmojisPreload v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetRecentEmojisPreload(v);
		} break;

		case dbiRecentStickers: {
			RecentStickerPreload v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetRecentStickersPreload(v);
		} break;

		case dbiEmojiVariants: {
			EmojiColorVariants v;
			stream >> v;
			if (!_checkStreamStatus(stream)) return false;

			cSetEmojiVariants(v);
		} break;

		case dbiDialogLastPath: {
			QString path;
			stream >> path;
			if (!_checkStreamStatus(stream)) return false;

			cSetDialogLastPath(path);
		} break;

		default:
			LOG(("App Error: unknown blockId in _readSetting: %1").arg(blockId));
			return false;
		}

		return true;
	}

	bool _readOldSettings(bool remove = true) {
		bool result = false;
		QFile file(cWorkingDir() + qsl("tdata/config"));
		if (file.open(QIODevice::ReadOnly)) {
			LOG(("App Info: reading old config.."));
			QDataStream stream(&file);
			stream.setVersion(QDataStream::Qt_5_1);

			qint32 version = 0;
			while (!stream.atEnd()) {
				quint32 blockId;
				stream >> blockId;
				if (!_checkStreamStatus(stream)) break;

				if (blockId == dbiVersion) {
					stream >> version;
					if (!_checkStreamStatus(stream)) break;

					if (version > AppVersion) break;
				} else if (!_readSetting(blockId, stream, version)) {
					break;
				}
			}
			file.close();
			result = true;
		}
		if (remove) file.remove();
		return result;
	}

	void _readOldUserSettingsFields(QIODevice *device, qint32 &version) {
		QDataStream stream(device);
		stream.setVersion(QDataStream::Qt_5_1);

		while (!stream.atEnd()) {
			quint32 blockId;
			stream >> blockId;
			if (!_checkStreamStatus(stream)) {
				break;
			}

			if (blockId == dbiVersion) {
				stream >> version;
				if (!_checkStreamStatus(stream)) {
					break;
				}

				if (version > AppVersion) return;
			} else if (blockId == dbiEncryptedWithSalt) {
				QByteArray salt, data, decrypted;
				stream >> salt >> data;
				if (!_checkStreamStatus(stream)) {
					break;
				}

				if (salt.size() != 32) {
					LOG(("App Error: bad salt in old user config encrypted part, size: %1").arg(salt.size()));
					continue;
				}

				createLocalKey(QByteArray(), &salt, &_oldKey);

				if (data.size() <= 16 || (data.size() & 0x0F)) {
					LOG(("App Error: bad encrypted part size in old user config: %1").arg(data.size()));
					continue;
				}
				uint32 fullDataLen = data.size() - 16;
				decrypted.resize(fullDataLen);
				const char *dataKey = data.constData(), *encrypted = data.constData() + 16;
				aesDecryptLocal(encrypted, decrypted.data(), fullDataLen, &_oldKey, dataKey);
				uchar sha1Buffer[20];
				if (memcmp(hashSha1(decrypted.constData(), decrypted.size(), sha1Buffer), dataKey, 16)) {
					LOG(("App Error: bad decrypt key, data from old user config not decrypted"));
					continue;
				}
				uint32 dataLen = *(const uint32*)decrypted.constData();
				if (dataLen > uint32(decrypted.size()) || dataLen <= fullDataLen - 16 || dataLen < 4) {
					LOG(("App Error: bad decrypted part size in old user config: %1, fullDataLen: %2, decrypted size: %3").arg(dataLen).arg(fullDataLen).arg(decrypted.size()));
					continue;
				}
				decrypted.resize(dataLen);
				QBuffer decryptedStream(&decrypted);
				decryptedStream.open(QIODevice::ReadOnly);
				decryptedStream.seek(4); // skip size
				LOG(("App Info: reading encrypted old user config.."));

				_readOldUserSettingsFields(&decryptedStream, version);
			} else if (!_readSetting(blockId, stream, version)) {
				return;
			}
		}
	}

	bool _readOldUserSettings(bool remove = true) {
		bool result = false;
		QFile file(cWorkingDir() + cDataFile() + (cTestMode() ? qsl("_test") : QString()) + qsl("_config"));
		if (file.open(QIODevice::ReadOnly)) {
			LOG(("App Info: reading old user config.."));
			qint32 version = 0;

			mtpDcOptions dcOpts;
			{
				QReadLocker lock(MTP::dcOptionsMutex());
				dcOpts = cDcOptions();
			}
			_dcOpts = &dcOpts;
			_readOldUserSettingsFields(&file, version);
			{
				QWriteLocker lock(MTP::dcOptionsMutex());
				cSetDcOptions(dcOpts);
			}

			file.close();
			result = true;
		}
		if (remove) file.remove();
		return result;
	}

	void _readOldMtpDataFields(QIODevice *device, qint32 &version) {
		QDataStream stream(device);
		stream.setVersion(QDataStream::Qt_5_1);

		while (!stream.atEnd()) {
			quint32 blockId;
			stream >> blockId;
			if (!_checkStreamStatus(stream)) {
				break;
			}

			if (blockId == dbiVersion) {
				stream >> version;
				if (!_checkStreamStatus(stream)) {
					break;
				}

				if (version > AppVersion) return;
			} else if (blockId == dbiEncrypted) {
				QByteArray data, decrypted;
				stream >> data;
				if (!_checkStreamStatus(stream)) {
					break;
				}

				if (!_oldKey.created()) {
					LOG(("MTP Error: reading old encrypted keys without old key!"));
					continue;
				}

				if (data.size() <= 16 || (data.size() & 0x0F)) {
					LOG(("MTP Error: bad encrypted part size in old keys: %1").arg(data.size()));
					continue;
				}
				uint32 fullDataLen = data.size() - 16;
				decrypted.resize(fullDataLen);
				const char *dataKey = data.constData(), *encrypted = data.constData() + 16;
				aesDecryptLocal(encrypted, decrypted.data(), fullDataLen, &_oldKey, dataKey);
				uchar sha1Buffer[20];
				if (memcmp(hashSha1(decrypted.constData(), decrypted.size(), sha1Buffer), dataKey, 16)) {
					LOG(("MTP Error: bad decrypt key, data from old keys not decrypted"));
					continue;
				}
				uint32 dataLen = *(const uint32*)decrypted.constData();
				if (dataLen > uint32(decrypted.size()) || dataLen <= fullDataLen - 16 || dataLen < 4) {
					LOG(("MTP Error: bad decrypted part size in old keys: %1, fullDataLen: %2, decrypted size: %3").arg(dataLen).arg(fullDataLen).arg(decrypted.size()));
					continue;
				}
				decrypted.resize(dataLen);
				QBuffer decryptedStream(&decrypted);
				decryptedStream.open(QIODevice::ReadOnly);
				decryptedStream.seek(4); // skip size
				LOG(("App Info: reading encrypted old keys.."));

				_readOldMtpDataFields(&decryptedStream, version);
			} else if (!_readSetting(blockId, stream, version)) {
				return;
			}
		}
	}

	bool _readOldMtpData(bool remove = true) {
		bool result = false;
		QFile file(cWorkingDir() + cDataFile() + (cTestMode() ? qsl("_test") : QString()));
		if (file.open(QIODevice::ReadOnly)) {
			LOG(("App Info: reading old keys.."));
			qint32 version = 0;

			mtpDcOptions dcOpts;
			{
				QReadLocker lock(MTP::dcOptionsMutex());
				dcOpts = cDcOptions();
			}
			_dcOpts = &dcOpts;
			_readOldMtpDataFields(&file, version);
			{
				QWriteLocker lock(MTP::dcOptionsMutex());
				cSetDcOptions(dcOpts);
			}

			file.close();
			result = true;
		}
		if (remove) file.remove();
		return result;
	}

	void _writeUserSettings() {
		if (!_userSettingsKey) {
			_userSettingsKey = genKey();
			_mapChanged = true;
			_writeMap(WriteMapFast);
		}

		uint32 size = 11 * (sizeof(quint32) + sizeof(qint32));
		size += sizeof(quint32) + _stringSize(cAskDownloadPath() ? QString() : cDownloadPath());
		size += sizeof(quint32) + sizeof(qint32) + (cRecentEmojisPreload().isEmpty() ? cGetRecentEmojis().size() : cRecentEmojisPreload().size()) * (sizeof(uint64) + sizeof(ushort));
		size += sizeof(quint32) + sizeof(qint32) + cEmojiVariants().size() * (sizeof(uint32) + sizeof(uint64));
		size += sizeof(quint32) + sizeof(qint32) + (cRecentStickersPreload().isEmpty() ? cGetRecentStickers().size() : cRecentStickersPreload().size()) * (sizeof(uint64) + sizeof(ushort));
		size += sizeof(quint32) + _stringSize(cDialogLastPath());

		EncryptedDescriptor data(size);
		data.stream << quint32(dbiSendKey) << qint32(cCtrlEnter() ? dbiskCtrlEnter : dbiskEnter);
		data.stream << quint32(dbiTileBackground) << qint32(cTileBackground() ? 1 : 0);
		data.stream << quint32(dbiAutoLock) << qint32(cAutoLock());
		data.stream << quint32(dbiReplaceEmojis) << qint32(cReplaceEmojis() ? 1 : 0);
		data.stream << quint32(dbiDefaultAttach) << qint32(cDefaultAttach());
		data.stream << quint32(dbiSoundNotify) << qint32(cSoundNotify());
		data.stream << quint32(dbiDesktopNotify) << qint32(cDesktopNotify());
		data.stream << quint32(dbiNotifyView) << qint32(cNotifyView());
		data.stream << quint32(dbiAskDownloadPath) << qint32(cAskDownloadPath());
		data.stream << quint32(dbiDownloadPath) << (cAskDownloadPath() ? QString() : cDownloadPath());
		data.stream << quint32(dbiCompressPastedImage) << qint32(cCompressPastedImage());
		data.stream << quint32(dbiEmojiTab) << qint32(cEmojiTab());
		data.stream << quint32(dbiDialogLastPath) << cDialogLastPath();

		{
			RecentEmojisPreload v(cRecentEmojisPreload());
			if (v.isEmpty()) {
				v.reserve(cGetRecentEmojis().size());
				for (RecentEmojiPack::const_iterator i = cGetRecentEmojis().cbegin(), e = cGetRecentEmojis().cend(); i != e; ++i) {
					v.push_back(qMakePair(emojiKey(i->first), i->second));
				}
			}
			data.stream << quint32(dbiRecentEmojis) << v;
		}
		data.stream << quint32(dbiEmojiVariants) << cEmojiVariants();
		{
			RecentStickerPreload v(cRecentStickersPreload());
			if (v.isEmpty()) {
				v.reserve(cGetRecentStickers().size());
				for (RecentStickerPack::const_iterator i = cGetRecentStickers().cbegin(), e = cGetRecentStickers().cend(); i != e; ++i) {
					v.push_back(qMakePair(i->first->id, i->second));
				}
			}
			data.stream << quint32(dbiRecentStickers) << v;
		}

		FileWriteDescriptor file(_userSettingsKey);
		file.writeEncrypted(data);
	}

	void _readUserSettings() {
		FileReadDescriptor userSettings;
		if (!readEncryptedFile(userSettings, _userSettingsKey)) {
			_readOldUserSettings();
			return _writeUserSettings();
		}

		LOG(("App Info: reading encrypted user settings.."));
		while (!userSettings.stream.atEnd()) {
			quint32 blockId;
			userSettings.stream >> blockId;
			if (!_checkStreamStatus(userSettings.stream)) {
				return _writeUserSettings();
			}

			if (!_readSetting(blockId, userSettings.stream, userSettings.version)) {
				return _writeUserSettings();
			}
		}
	}

	void _writeMtpData() {
		FileWriteDescriptor mtp(toFilePart(_dataNameKey), SafePath);
		if (!_localKey.created()) {
			LOG(("App Error: localkey not created in _writeMtpData()"));
			return;
		}

		mtpKeysMap keys = MTP::getKeys();

		quint32 size = sizeof(quint32) + sizeof(qint32) + sizeof(quint32);
		size += keys.size() * (sizeof(quint32) + sizeof(quint32) + 256);

		EncryptedDescriptor data(size);
		data.stream << quint32(dbiUser) << qint32(MTP::authedId()) << quint32(MTP::maindc());
		for (mtpKeysMap::const_iterator i = keys.cbegin(), e = keys.cend(); i != e; ++i) {
			data.stream << quint32(dbiKey) << quint32((*i)->getDC());
			(*i)->write(data.stream);
		}

		mtp.writeEncrypted(data, _localKey);
	}

	void _readMtpData() {
		FileReadDescriptor mtp;
		if (!readEncryptedFile(mtp, toFilePart(_dataNameKey), SafePath)) {
			if (_localKey.created()) {
				_readOldMtpData();
				_writeMtpData();
			}
			return;
		}

		LOG(("App Info: reading encrypted mtp data.."));
		while (!mtp.stream.atEnd()) {
			quint32 blockId;
			mtp.stream >> blockId;
			if (!_checkStreamStatus(mtp.stream)) {
				return _writeMtpData();
			}

			if (!_readSetting(blockId, mtp.stream, mtp.version)) {
				return _writeMtpData();
			}
		}
	}

	Local::ReadMapState _readMap(const QByteArray &pass) {
		uint64 ms = getms();
		QByteArray dataNameUtf8 = (cDataFile() + (cTestMode() ? qsl(":/test/") : QString())).toUtf8();
		FileKey dataNameHash[2];
		hashMd5(dataNameUtf8.constData(), dataNameUtf8.size(), dataNameHash);
		_dataNameKey = dataNameHash[0];
		_userBasePath = _basePath + toFilePart(_dataNameKey) + QChar('/');

		FileReadDescriptor mapData;
		if (!readFile(mapData, qsl("map"))) {
			return Local::ReadMapFailed;
		}
		LOG(("App Info: reading map.."));

		QByteArray salt, keyEncrypted, mapEncrypted;
		mapData.stream >> salt >> keyEncrypted >> mapEncrypted;
		if (!_checkStreamStatus(mapData.stream)) {
			return Local::ReadMapFailed;
		}

		if (salt.size() != LocalEncryptSaltSize) {
			LOG(("App Error: bad salt in map file, size: %1").arg(salt.size()));
			return Local::ReadMapFailed;
		}
		createLocalKey(pass, &salt, &_passKey);

		EncryptedDescriptor keyData, map;
		if (!decryptLocal(keyData, keyEncrypted, _passKey)) {
			LOG(("App Info: could not decrypt pass-protected key from map file, maybe bad password.."));
			return Local::ReadMapPassNeeded;
		}
		uchar key[LocalEncryptKeySize] = { 0 };
		if (keyData.stream.readRawData((char*)key, LocalEncryptKeySize) != LocalEncryptKeySize || !keyData.stream.atEnd()) {
			LOG(("App Error: could not read pass-protected key from map file"));
			return Local::ReadMapFailed;
		}
		_localKey.setKey(key);

		_passKeyEncrypted = keyEncrypted;
		_passKeySalt = salt;

		if (!decryptLocal(map, mapEncrypted)) {
			LOG(("App Error: could not decrypt map."));
			return Local::ReadMapFailed;
		}
		LOG(("App Info: reading encrypted map.."));

		DraftsMap draftsMap, draftsPositionsMap;
		DraftsNotReadMap draftsNotReadMap;
		StorageMap imagesMap, stickerImagesMap, audiosMap;
		qint64 storageImagesSize = 0, storageStickersSize = 0, storageAudiosSize = 0;
		quint64 locationsKey = 0, recentStickersKeyOld = 0, stickersKey = 0, backgroundKey = 0, userSettingsKey = 0, recentHashtagsKey = 0;
		while (!map.stream.atEnd()) {
			quint32 keyType;
			map.stream >> keyType;
			switch (keyType) {
			case lskDraft: {
				quint32 count = 0;
				map.stream >> count;
				for (quint32 i = 0; i < count; ++i) {
					FileKey key;
					quint64 p;
					map.stream >> key >> p;
					draftsMap.insert(p, key);
					draftsNotReadMap.insert(p, true);
				}
			} break;
			case lskDraftPosition: {
				quint32 count = 0;
				map.stream >> count;
				for (quint32 i = 0; i < count; ++i) {
					FileKey key;
					quint64 p;
					map.stream >> key >> p;
					draftsPositionsMap.insert(p, key);
				}
			} break;
			case lskImages: {
				quint32 count = 0;
				map.stream >> count;
				for (quint32 i = 0; i < count; ++i) {
					FileKey key;
					quint64 first, second;
					qint32 size;
					map.stream >> key >> first >> second >> size;
					imagesMap.insert(StorageKey(first, second), FileDesc(key, size));
					storageImagesSize += size;
				}
			} break;
			case lskStickerImages: {
				quint32 count = 0;
				map.stream >> count;
				for (quint32 i = 0; i < count; ++i) {
					FileKey key;
					quint64 first, second;
					qint32 size;
					map.stream >> key >> first >> second >> size;
					stickerImagesMap.insert(StorageKey(first, second), FileDesc(key, size));
					storageStickersSize += size;
				}
			} break;
			case lskAudios: {
				quint32 count = 0;
				map.stream >> count;
				for (quint32 i = 0; i < count; ++i) {
					FileKey key;
					quint64 first, second;
					qint32 size;
					map.stream >> key >> first >> second >> size;
					audiosMap.insert(StorageKey(first, second), FileDesc(key, size));
					storageAudiosSize += size;
				}
			} break;
			case lskLocations: {
				map.stream >> locationsKey;
			} break;
			case lskRecentStickersOld: {
				map.stream >> recentStickersKeyOld;
			} break;
			case lskBackground: {
				map.stream >> backgroundKey;
			} break;
			case lskUserSettings: {
				map.stream >> userSettingsKey;
			} break;
			case lskRecentHashtags: {
				map.stream >> recentHashtagsKey;
			} break;
			case lskStickers: {
				map.stream >> stickersKey;
			} break;
			default:
				LOG(("App Error: unknown key type in encrypted map: %1").arg(keyType));
				return Local::ReadMapFailed;
			}
			if (!_checkStreamStatus(map.stream)) {
				return Local::ReadMapFailed;
			}
		}

		_draftsMap = draftsMap;
		_draftsPositionsMap = draftsPositionsMap;
		_draftsNotReadMap = draftsNotReadMap;

		_imagesMap = imagesMap;
		_storageImagesSize = storageImagesSize;
		_stickerImagesMap = stickerImagesMap;
		_storageStickersSize = storageStickersSize;
		_audiosMap = audiosMap;
		_storageAudiosSize = storageAudiosSize;

		_locationsKey = locationsKey;
		_recentStickersKeyOld = recentStickersKeyOld;
		_stickersKey = stickersKey;
		_backgroundKey = backgroundKey;
		_userSettingsKey = userSettingsKey;
		_recentHashtagsKey = recentHashtagsKey;
		_oldMapVersion = mapData.version;
		if (_oldMapVersion < AppVersion) {
			_mapChanged = true;
			_writeMap();
		} else {
			_mapChanged = false;
		}

		if (_locationsKey) {
			_readLocations();
		}

		_readUserSettings();
		_readMtpData();

		LOG(("Map read time: %1").arg(getms() - ms));
		return Local::ReadMapDone;
	}

	void _writeMap(WriteMapWhen when) {
		if (when != WriteMapNow) {
			_manager->writeMap(when == WriteMapFast);
			return;
		}
		_manager->writingMap();
		if (!_mapChanged) return;
		if (_userBasePath.isEmpty()) {
			LOG(("App Error: _userBasePath is empty in writeMap()"));
			return;
		}

		if (!QDir().exists(_userBasePath)) QDir().mkpath(_userBasePath);

		FileWriteDescriptor map(qsl("map"));
		if (_passKeySalt.isEmpty() || _passKeyEncrypted.isEmpty()) {
			uchar local5Key[LocalEncryptKeySize] = { 0 };
			QByteArray pass(LocalEncryptKeySize, Qt::Uninitialized), salt(LocalEncryptSaltSize, Qt::Uninitialized);
			memset_rand(pass.data(), pass.size());
			memset_rand(salt.data(), salt.size());
			createLocalKey(pass, &salt, &_localKey);

			_passKeySalt.resize(LocalEncryptSaltSize);
			memset_rand(_passKeySalt.data(), _passKeySalt.size());
			createLocalKey(QByteArray(), &_passKeySalt, &_passKey);

			EncryptedDescriptor passKeyData(LocalEncryptKeySize);
			_localKey.write(passKeyData.stream);
			_passKeyEncrypted = FileWriteDescriptor::prepareEncrypted(passKeyData, _passKey);
		}
		map.writeData(_passKeySalt);
		map.writeData(_passKeyEncrypted);

		uint32 mapSize = 0;
		if (!_draftsMap.isEmpty()) mapSize += sizeof(quint32) * 2 + _draftsMap.size() * sizeof(quint64) * 2;
		if (!_draftsPositionsMap.isEmpty()) mapSize += sizeof(quint32) * 2 + _draftsPositionsMap.size() * sizeof(quint64) * 2;
		if (!_imagesMap.isEmpty()) mapSize += sizeof(quint32) * 2 + _imagesMap.size() * (sizeof(quint64) * 3 + sizeof(qint32));
		if (!_stickerImagesMap.isEmpty()) mapSize += sizeof(quint32) * 2 + _stickerImagesMap.size() * (sizeof(quint64) * 3 + sizeof(qint32));
		if (!_audiosMap.isEmpty()) mapSize += sizeof(quint32) * 2 + _audiosMap.size() * (sizeof(quint64) * 3 + sizeof(qint32));
		if (_locationsKey) mapSize += sizeof(quint32) + sizeof(quint64);
		if (_recentStickersKeyOld) mapSize += sizeof(quint32) + sizeof(quint64);
		if (_stickersKey) mapSize += sizeof(quint32) + sizeof(quint64);
		if (_backgroundKey) mapSize += sizeof(quint32) + sizeof(quint64);
		if (_userSettingsKey) mapSize += sizeof(quint32) + sizeof(quint64);
		if (_recentHashtagsKey) mapSize += sizeof(quint32) + sizeof(quint64);
		EncryptedDescriptor mapData(mapSize);
		if (!_draftsMap.isEmpty()) {
			mapData.stream << quint32(lskDraft) << quint32(_draftsMap.size());
			for (DraftsMap::const_iterator i = _draftsMap.cbegin(), e = _draftsMap.cend(); i != e; ++i) {
				mapData.stream << quint64(i.value()) << quint64(i.key());
			}
		}
		if (!_draftsPositionsMap.isEmpty()) {
			mapData.stream << quint32(lskDraftPosition) << quint32(_draftsPositionsMap.size());
			for (DraftsMap::const_iterator i = _draftsPositionsMap.cbegin(), e = _draftsPositionsMap.cend(); i != e; ++i) {
				mapData.stream << quint64(i.value()) << quint64(i.key());
			}
		}
		if (!_imagesMap.isEmpty()) {
			mapData.stream << quint32(lskImages) << quint32(_imagesMap.size());
			for (StorageMap::const_iterator i = _imagesMap.cbegin(), e = _imagesMap.cend(); i != e; ++i) {
				mapData.stream << quint64(i.value().first) << quint64(i.key().first) << quint64(i.key().second) << qint32(i.value().second);
			}
		}
		if (!_stickerImagesMap.isEmpty()) {
			mapData.stream << quint32(lskStickerImages) << quint32(_stickerImagesMap.size());
			for (StorageMap::const_iterator i = _stickerImagesMap.cbegin(), e = _stickerImagesMap.cend(); i != e; ++i) {
				mapData.stream << quint64(i.value().first) << quint64(i.key().first) << quint64(i.key().second) << qint32(i.value().second);
			}
		}
		if (!_audiosMap.isEmpty()) {
			mapData.stream << quint32(lskAudios) << quint32(_audiosMap.size());
			for (StorageMap::const_iterator i = _audiosMap.cbegin(), e = _audiosMap.cend(); i != e; ++i) {
				mapData.stream << quint64(i.value().first) << quint64(i.key().first) << quint64(i.key().second) << qint32(i.value().second);
			}
		}
		if (_locationsKey) {
			mapData.stream << quint32(lskLocations) << quint64(_locationsKey);
		}
		if (_recentStickersKeyOld) {
			mapData.stream << quint32(lskRecentStickersOld) << quint64(_recentStickersKeyOld);
		}
		if (_stickersKey) {
			mapData.stream << quint32(lskStickers) << quint64(_stickersKey);
		}
		if (_backgroundKey) {
			mapData.stream << quint32(lskBackground) << quint64(_backgroundKey);
		}
		if (_userSettingsKey) {
			mapData.stream << quint32(lskUserSettings) << quint64(_userSettingsKey);
		}
		if (_recentHashtagsKey) {
			mapData.stream << quint32(lskRecentHashtags) << quint64(_recentHashtagsKey);
		}
		map.writeEncrypted(mapData);

		_mapChanged = false;
	}

}

namespace _local_inner {

	Manager::Manager() {
		_mapWriteTimer.setSingleShot(true);
		connect(&_mapWriteTimer, SIGNAL(timeout()), this, SLOT(mapWriteTimeout()));
		_locationsWriteTimer.setSingleShot(true);
		connect(&_locationsWriteTimer, SIGNAL(timeout()), this, SLOT(locationsWriteTimeout()));
	}

	void Manager::writeMap(bool fast) {
		if (!_mapWriteTimer.isActive() || fast) {
			_mapWriteTimer.start(fast ? 1 : WriteMapTimeout);
		} else if (_mapWriteTimer.remainingTime() <= 0) {
			mapWriteTimeout();
		}
	}

	void Manager::writingMap() {
		_mapWriteTimer.stop();
	}

	void Manager::writeLocations(bool fast) {
		if (!_locationsWriteTimer.isActive() || fast) {
			_locationsWriteTimer.start(fast ? 1 : WriteMapTimeout);
		} else if (_locationsWriteTimer.remainingTime() <= 0) {
			locationsWriteTimeout();
		}
	}

	void Manager::writingLocations() {
		_locationsWriteTimer.stop();
	}

	void Manager::mapWriteTimeout() {
		_writeMap(WriteMapNow);
	}

	void Manager::locationsWriteTimeout() {
		_writeLocations(WriteMapNow);
	}

	void Manager::finish() {
		if (_mapWriteTimer.isActive()) {
			mapWriteTimeout();
		}
		if (_locationsWriteTimer.isActive()) {
			locationsWriteTimeout();
		}
	}

}

namespace Local {

	void start() {
		if (!_started) {
			_started = true;
			_manager = new _local_inner::Manager();
		}
	}

	void stop() {
		if (_manager) {
			_writeMap(WriteMapNow);
			_manager->finish();
			_manager->deleteLater();
			_manager = 0;
		}
	}

	void readSettings() {
		Local::start();

		_basePath = cWorkingDir() + qsl("tdata/");
		if (!QDir().exists(_basePath)) QDir().mkpath(_basePath);

		FileReadDescriptor settingsData;
		if (!readFile(settingsData, cTestMode() ? qsl("settings_test") : qsl("settings"), SafePath)) {
			_readOldSettings();
			_readOldUserSettings(false); // needed further in _readUserSettings
			_readOldMtpData(false); // needed further in _readMtpData
			return writeSettings();
		}
		LOG(("App Info: reading settings.."));

		QByteArray salt, settingsEncrypted;
		settingsData.stream >> salt >> settingsEncrypted;
		if (!_checkStreamStatus(settingsData.stream)) {
			return writeSettings();
		}

		if (salt.size() != LocalEncryptSaltSize) {
			LOG(("App Error: bad salt in settings file, size: %1").arg(salt.size()));
			return writeSettings();
		}
		createLocalKey(QByteArray(), &salt, &_settingsKey);

		EncryptedDescriptor settings;
		if (!decryptLocal(settings, settingsEncrypted, _settingsKey)) {
			LOG(("App Error: could not decrypt settings from settings file, maybe bad passcode.."));
			return writeSettings();
		}
		mtpDcOptions dcOpts;
		{
			QReadLocker lock(MTP::dcOptionsMutex());
			dcOpts = cDcOptions();
		}
		_dcOpts = &dcOpts;
		LOG(("App Info: reading encrypted settings.."));
		while (!settings.stream.atEnd()) {
			quint32 blockId;
			settings.stream >> blockId;
			if (!_checkStreamStatus(settings.stream)) {
				return writeSettings();
			}

			if (!_readSetting(blockId, settings.stream, settingsData.version)) {
				return writeSettings();
			}
		}
		if (dcOpts.isEmpty()) {
			const BuiltInDc *bdcs = builtInDcs();
			for (int i = 0, l = builtInDcsCount(); i < l; ++i) {
				dcOpts.insert(bdcs[i].id, mtpDcOption(bdcs[i].id, "", bdcs[i].ip, bdcs[i].port));
				DEBUG_LOG(("MTP Info: adding built in DC %1 connect option: %2:%3").arg(bdcs[i].id).arg(bdcs[i].ip).arg(bdcs[i].port));
			}
		}
		{
			QWriteLocker lock(MTP::dcOptionsMutex());
			cSetDcOptions(dcOpts);
		}

		_settingsSalt = salt;
	}

	void writeSettings() {
		if (_basePath.isEmpty()) {
			LOG(("App Error: _basePath is empty in writeSettings()"));
			return;
		}

		if (!QDir().exists(_basePath)) QDir().mkpath(_basePath);

		FileWriteDescriptor settings(cTestMode() ? qsl("settings_test") : qsl("settings"), SafePath);
		if (_settingsSalt.isEmpty() || !_settingsKey.created()) {
			_settingsSalt.resize(LocalEncryptSaltSize);
			memset_rand(_settingsSalt.data(), _settingsSalt.size());
			createLocalKey(QByteArray(), &_settingsSalt, &_settingsKey);
		}
		settings.writeData(_settingsSalt);

		mtpDcOptions dcOpts;
		{
			QReadLocker lock(MTP::dcOptionsMutex());
			dcOpts = cDcOptions();
		}
		if (dcOpts.isEmpty()) {
			const BuiltInDc *bdcs = builtInDcs();
			for (int i = 0, l = builtInDcsCount(); i < l; ++i) {
				dcOpts.insert(bdcs[i].id, mtpDcOption(bdcs[i].id, "", bdcs[i].ip, bdcs[i].port));
				DEBUG_LOG(("MTP Info: adding built in DC %1 connect option: %2:%3").arg(bdcs[i].id).arg(bdcs[i].ip).arg(bdcs[i].port));
			}

			QWriteLocker lock(MTP::dcOptionsMutex());
			cSetDcOptions(dcOpts);
		}

		quint32 size = 10 * (sizeof(quint32) + sizeof(qint32));
		for (mtpDcOptions::const_iterator i = dcOpts.cbegin(), e = dcOpts.cend(); i != e; ++i) {
			size += sizeof(quint32) + sizeof(quint32) + sizeof(quint32);
			size += _stringSize(QString::fromUtf8(i->host.data(), i->host.size())) + _stringSize(QString::fromUtf8(i->ip.data(), i->ip.size()));
		}
		size += sizeof(quint32) + _stringSize(cLangFile());

		size += sizeof(quint32) + sizeof(qint32);
		if (cConnectionType() == dbictHttpProxy || cConnectionType() == dbictTcpProxy) {
			const ConnectionProxy &proxy(cConnectionProxy());
			size += _stringSize(proxy.host) + sizeof(qint32) + _stringSize(proxy.user) + _stringSize(proxy.password);
		}

		size += sizeof(quint32) + sizeof(qint32) * 6;

		EncryptedDescriptor data(size);
		data.stream << quint32(dbiMaxGroupCount) << qint32(cMaxGroupCount());
		data.stream << quint32(dbiAutoStart) << qint32(cAutoStart());
		data.stream << quint32(dbiStartMinimized) << qint32(cStartMinimized());
		data.stream << quint32(dbiSendToMenu) << qint32(cSendToMenu());
		data.stream << quint32(dbiWorkMode) << qint32(cWorkMode());
		data.stream << quint32(dbiSeenTrayTooltip) << qint32(cSeenTrayTooltip());
		data.stream << quint32(dbiAutoUpdate) << qint32(cAutoUpdate());
		data.stream << quint32(dbiLastUpdateCheck) << qint32(cLastUpdateCheck());
		data.stream << quint32(dbiScale) << qint32(cConfigScale());
		data.stream << quint32(dbiLang) << qint32(cLang());
		for (mtpDcOptions::const_iterator i = dcOpts.cbegin(), e = dcOpts.cend(); i != e; ++i) {
			data.stream << quint32(dbiDcOption) << quint32(i->id);
			data.stream << QString::fromUtf8(i->host.data(), i->host.size()) << QString::fromUtf8(i->ip.data(), i->ip.size());
			data.stream << quint32(i->port);
		}			
		data.stream << quint32(dbiLangFile) << cLangFile();

		data.stream << quint32(dbiConnectionType) << qint32(cConnectionType());
		if (cConnectionType() == dbictHttpProxy || cConnectionType() == dbictTcpProxy) {
			const ConnectionProxy &proxy(cConnectionProxy());
			data.stream << proxy.host << qint32(proxy.port) << proxy.user << proxy.password;
		}

		TWindowPos pos(cWindowPos());
		data.stream << quint32(dbiWindowPosition) << qint32(pos.x) << qint32(pos.y) << qint32(pos.w) << qint32(pos.h) << qint32(pos.moncrc) << qint32(pos.maximized);

		settings.writeEncrypted(data, _settingsKey);
	}

	void writeUserSettings() {
		_writeUserSettings();
	}

	void writeMtpData() {
		_writeMtpData();
	}

	void reset() {
		_passKeySalt.clear(); // reset passcode, local key
		_draftsMap.clear();
		_draftsPositionsMap.clear();
		_imagesMap.clear();
		_draftsNotReadMap.clear();
		_stickerImagesMap.clear();
		_audiosMap.clear();
		_locationsKey = _recentStickersKeyOld = _stickersKey = _backgroundKey = _userSettingsKey = _recentHashtagsKey = 0;
		_mapChanged = true;
		_writeMap(WriteMapNow);

		_writeMtpData();
	}

	bool checkPasscode(const QByteArray &passcode) {
		mtpAuthKey tmp;
		createLocalKey(passcode, &_passKeySalt, &tmp);
		return (tmp == _passKey);
	}

	void setPasscode(const QByteArray &passcode) {
		createLocalKey(passcode, &_passKeySalt, &_passKey);

		EncryptedDescriptor passKeyData(LocalEncryptKeySize);
		_localKey.write(passKeyData.stream);
		_passKeyEncrypted = FileWriteDescriptor::prepareEncrypted(passKeyData, _passKey);

		_mapChanged = true;
		_writeMap(WriteMapNow);

		cSetHasPasscode(!passcode.isEmpty());
	}

	ReadMapState readMap(const QByteArray &pass) {
		ReadMapState result = _readMap(pass);
		if (result == ReadMapFailed) {
			_mapChanged = true;
			_writeMap(WriteMapNow);
		}
		return result;
	}

	int32 oldMapVersion() {
		return _oldMapVersion;
	}

	void writeDraft(const PeerId &peer, const MessageDraft &draft) {
		if (!_working()) return;

		if (draft.replyTo <= 0 && draft.text.isEmpty()) {
			DraftsMap::iterator i = _draftsMap.find(peer);
			if (i != _draftsMap.cend()) {
				clearKey(i.value());
				_draftsMap.erase(i);
				_mapChanged = true;
				_writeMap();
			}

			_draftsNotReadMap.remove(peer);
		} else {
			DraftsMap::const_iterator i = _draftsMap.constFind(peer);
			if (i == _draftsMap.cend()) {
				i = _draftsMap.insert(peer, genKey());
				_mapChanged = true;
				_writeMap(WriteMapFast);
			}
			EncryptedDescriptor data(sizeof(quint64) + _stringSize(draft.text) + sizeof(qint32));
			data.stream << quint64(peer) << draft.text << qint32(draft.replyTo) << qint32(draft.previewCancelled ? 1 : 0);
			FileWriteDescriptor file(i.value());
			file.writeEncrypted(data);

			_draftsNotReadMap.remove(peer);
		}
	}

	MessageDraft readDraft(const PeerId &peer) {
		if (!_draftsNotReadMap.remove(peer)) return MessageDraft();

		DraftsMap::iterator j = _draftsMap.find(peer);
		if (j == _draftsMap.cend()) {
			return MessageDraft();
		}
		FileReadDescriptor draft;
		if (!readEncryptedFile(draft, j.value())) {
			clearKey(j.value());
			_draftsMap.erase(j);
			return MessageDraft();
		}

		quint64 draftPeer;
		QString draftText;
		qint32 draftReplyTo = 0, draftPreviewCancelled = 0;
		draft.stream >> draftPeer >> draftText;
		if (draft.version >= 7021) draft.stream >> draftReplyTo;
		if (draft.version >= 8001) draft.stream >> draftPreviewCancelled;
		return (draftPeer == peer) ? MessageDraft(MsgId(draftReplyTo), draftText, (draftPreviewCancelled == 1)) : MessageDraft();
	}

	void writeDraftPositions(const PeerId &peer, const MessageCursor &cur) {
		if (!_working()) return;

		if (cur.position == 0 && cur.anchor == 0 && cur.scroll == 0) {
			DraftsMap::iterator i = _draftsPositionsMap.find(peer);
			if (i != _draftsPositionsMap.cend()) {
				clearKey(i.value());
				_draftsPositionsMap.erase(i);
				_mapChanged = true;
				_writeMap();
			}
		} else {
			DraftsMap::const_iterator i = _draftsPositionsMap.constFind(peer);
			if (i == _draftsPositionsMap.cend()) {
				i = _draftsPositionsMap.insert(peer, genKey());
				_mapChanged = true;
				_writeMap(WriteMapFast);
			}
			EncryptedDescriptor data(sizeof(quint64) + sizeof(qint32) * 3);
			data.stream << quint64(peer) << qint32(cur.position) << qint32(cur.anchor) << qint32(cur.scroll);
			FileWriteDescriptor file(i.value());
			file.writeEncrypted(data);
		}
	}

	MessageCursor readDraftPositions(const PeerId &peer) {
		DraftsMap::iterator j = _draftsPositionsMap.find(peer);
		if (j == _draftsPositionsMap.cend()) {
			return MessageCursor();
		}
		FileReadDescriptor draft;
		if (!readEncryptedFile(draft, j.value())) {
			clearKey(j.value());
			_draftsPositionsMap.erase(j);
			return MessageCursor();
		}

		quint64 draftPeer;
		qint32 curPosition, curAnchor, curScroll;
		draft.stream >> draftPeer >> curPosition >> curAnchor >> curScroll;

		return (draftPeer == peer) ? MessageCursor(curPosition, curAnchor, curScroll) : MessageCursor();
	}

	bool hasDraftPositions(const PeerId &peer) {
		return (_draftsPositionsMap.constFind(peer) != _draftsPositionsMap.cend());
	}

	void writeFileLocation(const MediaKey &location, const FileLocation &local) {
		if (local.name.isEmpty()) return;

		FileLocationPairs::iterator i = _fileLocationPairs.find(local.name);
		if (i != _fileLocationPairs.cend()) {
			if (i.value().second == local) {
				return;
			}
			if (i.value().first != location) {
				for (FileLocations::iterator j = _fileLocations.find(i.value().first), e = _fileLocations.end(); (j != e) && (j.key() == i.value().first);) {
					if (j.value() == i.value().second) {
						_fileLocations.erase(j);
						break;
					}
				}
				_fileLocationPairs.erase(i);
			}
		}
		_fileLocations.insert(location, local);
		_fileLocationPairs.insert(local.name, FileLocationPair(location, local));
		_writeLocations(WriteMapFast);
	}

	FileLocation readFileLocation(const MediaKey &location, bool check) {
		FileLocations::iterator i = _fileLocations.find(location);
		for (FileLocations::iterator i = _fileLocations.find(location); (i != _fileLocations.end()) && (i.key() == location);) {
			if (check) {
				QFileInfo info(i.value().name);
				if (!info.exists() || info.lastModified() != i.value().modified || info.size() != i.value().size) {
					_fileLocationPairs.remove(i.value().name);
					i = _fileLocations.erase(i);
					_writeLocations();
					continue;
				}
			}
			return i.value();
		}
		return FileLocation();
	}

	qint32 _storageImageSize(qint32 rawlen) {
		// fulllen + storagekey + type + len + data
		qint32 result = sizeof(uint32) + sizeof(quint64) * 2 + sizeof(quint32) + sizeof(quint32) + rawlen;
		if (result & 0x0F) result += 0x10 - (result & 0x0F);
		result += tdfMagicLen + sizeof(qint32) + sizeof(quint32) + 0x10 + 0x10; // magic + version + len of encrypted + part of sha1 + md5
		return result;
	}

	qint32 _storageStickerSize(qint32 rawlen) {
		// fulllen + storagekey + len + data
		qint32 result = sizeof(uint32) + sizeof(quint64) * 2 + sizeof(quint32) + rawlen;
		if (result & 0x0F) result += 0x10 - (result & 0x0F);
		result += tdfMagicLen + sizeof(qint32) + sizeof(quint32) + 0x10 + 0x10; // magic + version + len of encrypted + part of sha1 + md5
		return result;
	}

	qint32 _storageAudioSize(qint32 rawlen) {
		// fulllen + storagekey + len + data
		qint32 result = sizeof(uint32) + sizeof(quint64) * 2 + sizeof(quint32) + rawlen;
		if (result & 0x0F) result += 0x10 - (result & 0x0F);
		result += tdfMagicLen + sizeof(qint32) + sizeof(quint32) + 0x10 + 0x10; // magic + version + len of encrypted + part of sha1 + md5
		return result;
	}

	void writeImage(const StorageKey &location, const ImagePtr &image) {
		if (image->isNull() || !image->loaded()) return;
		if (_imagesMap.constFind(location) != _imagesMap.cend()) return;

		QByteArray fmt = image->savedFormat();
		StorageFileType format = StorageFileUnknown;
		if (fmt == "JPG") {
			format = StorageFileJpeg;
		} else if (fmt == "PNG") {
			format = StorageFilePng;
		} else if (fmt == "GIF") {
			format = StorageFileGif;
		}
		if (format) {
			image->forget();
			writeImage(location, StorageImageSaved(format, image->savedData()), false);
		}
	}

	void writeImage(const StorageKey &location, const StorageImageSaved &image, bool overwrite) {
		if (!_working()) return;

		qint32 size = _storageImageSize(image.data.size());
		StorageMap::const_iterator i = _imagesMap.constFind(location);
		if (i == _imagesMap.cend()) {
			i = _imagesMap.insert(location, FileDesc(genKey(UserPath), size));
			_storageImagesSize += size;
			_mapChanged = true;
			_writeMap();
		} else if (!overwrite) {
			return;
		}
		EncryptedDescriptor data(sizeof(quint64) * 2 + sizeof(quint32) + sizeof(quint32) + image.data.size());
		data.stream << quint64(location.first) << quint64(location.second) << quint32(image.type) << image.data;
		FileWriteDescriptor file(i.value().first, UserPath);
		file.writeEncrypted(data);
		if (i.value().second != size) {
			_storageImagesSize += size;
			_storageImagesSize -= i.value().second;
			_imagesMap[location].second = size;
		}
	}

	StorageImageSaved readImage(const StorageKey &location) {
		StorageMap::iterator j = _imagesMap.find(location);
		if (j == _imagesMap.cend()) {
			return StorageImageSaved();
		}
		FileReadDescriptor draft;
		if (!readEncryptedFile(draft, j.value().first, UserPath)) {
			clearKey(j.value().first, UserPath);
			_storageImagesSize -= j.value().second;
			_imagesMap.erase(j);
			return StorageImageSaved();
		}

		QByteArray imageData;
		quint64 locFirst, locSecond;
		quint32 imageType;
		draft.stream >> locFirst >> locSecond >> imageType >> imageData;

		return (locFirst == location.first && locSecond == location.second) ? StorageImageSaved(StorageFileType(imageType), imageData) : StorageImageSaved();
	}

	int32 hasImages() {
		return _imagesMap.size();
	}

	qint64 storageImagesSize() {
		return _storageImagesSize;
	}

	void writeStickerImage(const StorageKey &location, const QByteArray &sticker, bool overwrite) {
		if (!_working()) return;

		qint32 size = _storageStickerSize(sticker.size());
		StorageMap::const_iterator i = _stickerImagesMap.constFind(location);
		if (i == _stickerImagesMap.cend()) {
			i = _stickerImagesMap.insert(location, FileDesc(genKey(UserPath), size));
			_storageStickersSize += size;
			_mapChanged = true;
			_writeMap();
		} else if (!overwrite) {
			return;
		}
		EncryptedDescriptor data(sizeof(quint64) * 2 + sizeof(quint32) + sizeof(quint32) + sticker.size());
		data.stream << quint64(location.first) << quint64(location.second) << sticker;
		FileWriteDescriptor file(i.value().first, UserPath);
		file.writeEncrypted(data);
		if (i.value().second != size) {
			_storageStickersSize += size;
			_storageStickersSize -= i.value().second;
			_stickerImagesMap[location].second = size;
		}
	}

	QByteArray readStickerImage(const StorageKey &location) {
		StorageMap::iterator j = _stickerImagesMap.find(location);
		if (j == _stickerImagesMap.cend()) {
			return QByteArray();
		}
		FileReadDescriptor draft;
		if (!readEncryptedFile(draft, j.value().first, UserPath)) {
			clearKey(j.value().first, UserPath);
			_storageStickersSize -= j.value().second;
			_stickerImagesMap.erase(j);
			return QByteArray();
		}

		QByteArray stickerData;
		quint64 locFirst, locSecond;
		draft.stream >> locFirst >> locSecond >> stickerData;

		return (locFirst == location.first && locSecond == location.second) ? stickerData : QByteArray();
	}

	int32 hasStickers() {
		return _stickerImagesMap.size();
	}

	qint64 storageStickersSize() {
		return _storageStickersSize;
	}

	void writeAudio(const StorageKey &location, const QByteArray &audio, bool overwrite) {
		if (!_working()) return;

		qint32 size = _storageAudioSize(audio.size());
		StorageMap::const_iterator i = _audiosMap.constFind(location);
		if (i == _audiosMap.cend()) {
			i = _audiosMap.insert(location, FileDesc(genKey(UserPath), size));
			_storageAudiosSize += size;
			_mapChanged = true;
			_writeMap();
		} else if (!overwrite) {
			return;
		}
		EncryptedDescriptor data(sizeof(quint64) * 2 + sizeof(quint32) + sizeof(quint32) + audio.size());
		data.stream << quint64(location.first) << quint64(location.second) << audio;
		FileWriteDescriptor file(i.value().first, UserPath);
		file.writeEncrypted(data);
		if (i.value().second != size) {
			_storageAudiosSize += size;
			_storageAudiosSize -= i.value().second;
			_audiosMap[location].second = size;
		}
	}

	QByteArray readAudio(const StorageKey &location) {
		StorageMap::iterator j = _audiosMap.find(location);
		if (j == _audiosMap.cend()) {
			return QByteArray();
		}
		FileReadDescriptor draft;
		if (!readEncryptedFile(draft, j.value().first, UserPath)) {
			clearKey(j.value().first, UserPath);
			_storageAudiosSize -= j.value().second;
			_audiosMap.erase(j);
			return QByteArray();
		}

		QByteArray audioData;
		quint64 locFirst, locSecond;
		draft.stream >> locFirst >> locSecond >> audioData;

		return (locFirst == location.first && locSecond == location.second) ? audioData : QByteArray();
	}

	int32 hasAudios() {
		return _audiosMap.size();
	}

	qint64 storageAudiosSize() {
		return _storageAudiosSize;
	}

	void _writeStickerSet(QDataStream &stream, uint64 setId) {
		StickerSets::const_iterator it = cStickerSets().constFind(setId);
		if (it == cStickerSets().cend() || it->stickers.isEmpty()) return;

		stream << quint64(it->id) << quint64(it->access) << it->title << it->shortName << quint32(it->stickers.size());
		for (StickerPack::const_iterator j = it->stickers.cbegin(), e = it->stickers.cend(); j != e; ++j) {
			DocumentData *doc = *j;
			stream << quint64(doc->id) << quint64(doc->access) << qint32(doc->date) << doc->name << doc->mime << qint32(doc->dc) << qint32(doc->size) << qint32(doc->dimensions.width()) << qint32(doc->dimensions.height()) << qint32(doc->type) << doc->sticker->alt;
			switch (doc->sticker->set.type()) {
			case mtpc_inputStickerSetID: {
				stream << qint32(StickerSetTypeID);
			} break;
			case mtpc_inputStickerSetShortName: {
				stream << qint32(StickerSetTypeShortName);
			} break;
			case mtpc_inputStickerSetEmpty:
			default: {
				stream << qint32(StickerSetTypeEmpty);
			} break;
			}
			const StorageImageLocation &loc(doc->sticker->loc);
			stream << qint32(loc.width) << qint32(loc.height) << qint32(loc.dc) << quint64(loc.volume) << qint32(loc.local) << quint64(loc.secret);
		}
	}

	void writeStickers() {
		if (!_working()) return;

		const StickerSets &sets(cStickerSets());
		if (sets.isEmpty()) {
			if (_stickersKey) {
				clearKey(_stickersKey);
				_stickersKey = 0;
				_mapChanged = true;
			}
			_writeMap();
		} else {
			if (!_stickersKey) {
				_stickersKey = genKey();
				_mapChanged = true;
				_writeMap(WriteMapFast);
			}
			quint32 size = sizeof(quint32) + _bytearraySize(cStickersHash());
			for (StickerSets::const_iterator i = sets.cbegin(); i != sets.cend(); ++i) {
				if (i->stickers.isEmpty()) continue;

				// id + access + title + shortName + stickersCount
				size += sizeof(quint64) * 2 + _stringSize(i->title) + _stringSize(i->shortName) + sizeof(quint32);
				for (StickerPack::const_iterator j = i->stickers.cbegin(), e = i->stickers.cend(); j != e; ++j) {
					DocumentData *doc = *j;

					// id + access + date + namelen + name + mimelen + mime + dc + size + width + height + type + alt + type-of-set
					size += sizeof(quint64) + sizeof(quint64) + sizeof(qint32) + _stringSize(doc->name) + _stringSize(doc->mime) + sizeof(qint32) + sizeof(qint32) + sizeof(qint32) + sizeof(qint32) + sizeof(qint32) + _stringSize(doc->sticker->alt) + sizeof(qint32);

					// thumb-width + thumb-height + thumb-dc + thumb-volume + thumb-local + thumb-secret
					size += sizeof(qint32) + sizeof(qint32) + sizeof(qint32) + sizeof(quint64) + sizeof(qint32) + sizeof(quint64);
				}
			}
			EncryptedDescriptor data(size);
			data.stream << quint32(cStickerSetsOrder().size()) << cStickersHash();
			_writeStickerSet(data.stream, DefaultStickerSetId);
			_writeStickerSet(data.stream, CustomStickerSetId);
			for (StickerSetsOrder::const_iterator i = cStickerSetsOrder().cbegin(), e = cStickerSetsOrder().cend(); i != e; ++i) {
				_writeStickerSet(data.stream, *i);
			}
			FileWriteDescriptor file(_stickersKey);
			file.writeEncrypted(data);
		}
	}

	void importOldRecentStickers() {
		if (!_recentStickersKeyOld) return;

		FileReadDescriptor stickers;
		if (!readEncryptedFile(stickers, _recentStickersKeyOld)) {
			clearKey(_recentStickersKeyOld);
			_recentStickersKeyOld = 0;
			_writeMap();
			return;
		}
		
		StickerSets &sets(cRefStickerSets());
		sets.clear();
		cSetStickerSetsOrder(StickerSetsOrder());

		RecentStickerPack &recent(cRefRecentStickers());
		recent.clear();

		cSetStickersHash(QByteArray());

		StickerSet &def(sets.insert(DefaultStickerSetId, StickerSet(DefaultStickerSetId, 0, lang(lng_stickers_default_set), QString())).value());
		StickerSet &custom(sets.insert(CustomStickerSetId, StickerSet(CustomStickerSetId, 0, lang(lng_custom_stickers), QString())).value());

		QMap<uint64, bool> read;
		while (!stickers.stream.atEnd()) {
			quint64 id, access;
			QString name, mime, alt;
			qint32 date, dc, size, width, height, type;
			qint16 value;
			stickers.stream >> id >> value >> access >> date >> name >> mime >> dc >> size >> width >> height >> type;
			if (stickers.version >= 7021) {
				stickers.stream >> alt;
			}
			if (!value || read.contains(id)) continue;
			read.insert(id, true);

			QVector<MTPDocumentAttribute> attributes;
			if (!name.isEmpty()) attributes.push_back(MTP_documentAttributeFilename(MTP_string(name)));
			if (type == AnimatedDocument) {
				attributes.push_back(MTP_documentAttributeAnimated());
			} else if (type == StickerDocument) {
				attributes.push_back(MTP_documentAttributeSticker(MTP_string(alt), MTP_inputStickerSetEmpty()));
			}
			if (width > 0 && height > 0) {
				attributes.push_back(MTP_documentAttributeImageSize(MTP_int(width), MTP_int(height)));
			}

			DocumentData *doc = App::documentSet(id, 0, access, date, attributes, mime, ImagePtr(), dc, size, StorageImageLocation());
			if (!doc->sticker) continue;

			if (value > 0) {
				def.stickers.push_back(doc);
			} else {
				custom.stickers.push_back(doc);
			}
			if (recent.size() < StickerPanPerRow * StickerPanRowsPerPage && qAbs(value) > 1) recent.push_back(qMakePair(doc, qAbs(value)));
		}
		if (def.stickers.isEmpty()) sets.remove(DefaultStickerSetId);
		if (custom.stickers.isEmpty()) sets.remove(CustomStickerSetId);

		writeStickers();
		writeUserSettings();

		clearKey(_recentStickersKeyOld);
		_recentStickersKeyOld = 0;
		_writeMap();
	}

	void readStickers() {
		if (!_stickersKey) {
			return importOldRecentStickers();
		}

		FileReadDescriptor stickers;
		if (!readEncryptedFile(stickers, _stickersKey)) {
			clearKey(_stickersKey);
			_stickersKey = 0;
			_writeMap();
			return;
		}

		StickerSets &sets(cRefStickerSets());
		sets.clear();

		StickerSetsOrder &order(cRefStickerSetsOrder());
		order.clear();

		quint32 cnt;
		QByteArray hash;
		stickers.stream >> cnt >> hash;
		for (uint32 i = 0; i < cnt; ++i) {
			quint64 setId = 0, setAccess = 0;
			QString setTitle, setShortName;
			quint32 scnt = 0;
			stickers.stream >> setId >> setAccess >> setTitle >> setShortName >> scnt;

			if (setId == DefaultStickerSetId) {
				setTitle = lang(lng_stickers_default_set);
			} else if (setId == CustomStickerSetId) {
				setTitle = lang(lng_custom_stickers);
			} else {
				order.push_back(setId);
			}
			StickerSet &set(sets.insert(setId, StickerSet(setId, setAccess, setTitle, setShortName)).value());
			set.stickers.reserve(scnt);

			QMap<uint64, bool> read;
			for (uint32 j = 0; j < scnt; ++j) {
				quint64 id, access;
				QString name, mime, alt;
				qint32 date, dc, size, width, height, type, typeOfSet;
				stickers.stream >> id >> access >> date >> name >> mime >> dc >> size >> width >> height >> type >> alt >> typeOfSet;

				qint32 thumbWidth, thumbHeight, thumbDc, thumbLocal;
				quint64 thumbVolume, thumbSecret;
				stickers.stream >> thumbWidth >> thumbHeight >> thumbDc >> thumbVolume >> thumbLocal >> thumbSecret;

				if (read.contains(id)) continue;
				read.insert(id, true);

				if (setId == DefaultStickerSetId || setId == CustomStickerSetId) {
					typeOfSet = StickerSetTypeEmpty;
				}

				QVector<MTPDocumentAttribute> attributes;
				if (!name.isEmpty()) attributes.push_back(MTP_documentAttributeFilename(MTP_string(name)));
				if (type == AnimatedDocument) {
					attributes.push_back(MTP_documentAttributeAnimated());
				} else if (type == StickerDocument) {
					switch (typeOfSet) {
					case StickerSetTypeID: {
						attributes.push_back(MTP_documentAttributeSticker(MTP_string(alt), MTP_inputStickerSetID(MTP_long(setId), MTP_long(setAccess))));
					} break;
					case StickerSetTypeShortName: {
						attributes.push_back(MTP_documentAttributeSticker(MTP_string(alt), MTP_inputStickerSetShortName(MTP_string(setShortName))));
					} break;
					case StickerSetTypeEmpty:
					default: {
						attributes.push_back(MTP_documentAttributeSticker(MTP_string(alt), MTP_inputStickerSetEmpty()));
					} break;
					}
				}
				if (width > 0 && height > 0) {
					attributes.push_back(MTP_documentAttributeImageSize(MTP_int(width), MTP_int(height)));
				}

				StorageImageLocation thumb(thumbWidth, thumbHeight, thumbDc, thumbVolume, thumbLocal, thumbSecret);
				DocumentData *doc = App::documentSet(id, 0, access, date, attributes, mime, thumb.dc ? ImagePtr(thumb) : ImagePtr(), dc, size, thumb);
				if (!doc->sticker) continue;

				set.stickers.push_back(doc);
			}
		}

		cSetStickersHash(hash);
	}

	void writeBackground(int32 id, const QImage &img) {
		if (!_working()) return;

		QByteArray png;
		if (!img.isNull()) {
			QBuffer buf(&png);
			if (!img.save(&buf, "BMP")) return;
		}
		if (!_backgroundKey) {
			_backgroundKey = genKey();
			_mapChanged = true;
			_writeMap(WriteMapFast);
		}
		quint32 size = sizeof(qint32) + sizeof(quint32) + (png.isEmpty() ? 0 : (sizeof(quint32) + png.size()));
		EncryptedDescriptor data(size);
		data.stream << qint32(id);
		if (!png.isEmpty()) data.stream << png;

		FileWriteDescriptor file(_backgroundKey);
		file.writeEncrypted(data);
	}

	bool readBackground() {
		if (_backgroundWasRead) return false;
		_backgroundWasRead = true;

		FileReadDescriptor bg;
		if (!readEncryptedFile(bg, _backgroundKey)) {
			clearKey(_backgroundKey);
			_backgroundKey = 0;
			_writeMap();
			return false;
		}

		QByteArray pngData;
		qint32 id;
		bg.stream >> id;
		if (!id || id == DefaultChatBackground) {
			if (bg.version < 8005) {
				if (!id) cSetTileBackground(!DefaultChatBackground);
				App::initBackground(DefaultChatBackground, QImage(), true);
			} else {
				App::initBackground(id, QImage(), true);
			}
			return true;
		}
		bg.stream >> pngData;

		QImage img;
		QBuffer buf(&pngData);
		QImageReader reader(&buf);
		if (reader.read(&img)) {
			App::initBackground(id, img, true);
			return true;
		}
		return false;
	}

	void writeRecentHashtags() {
		if (!_working()) return;

		const RecentHashtagPack &write(cRecentWriteHashtags()), &search(cRecentSearchHashtags());
		if (write.isEmpty() && search.isEmpty()) readRecentHashtags();
		if (write.isEmpty() && search.isEmpty()) {
			if (_recentHashtagsKey) {
				clearKey(_recentHashtagsKey);
				_recentHashtagsKey = 0;
				_mapChanged = true;
			}
			_writeMap();
		} else {
			if (!_recentHashtagsKey) {
				_recentHashtagsKey = genKey();
				_mapChanged = true;
				_writeMap(WriteMapFast);
			}
			quint32 size = sizeof(quint32) * 2, writeCnt = 0, searchCnt = 0;
			for (RecentHashtagPack::const_iterator i = write.cbegin(); i != write.cend(); ++i) {
				if (!i->first.isEmpty()) {
					size += _stringSize(i->first) + sizeof(quint16);
					++writeCnt;
				}
			}
			for (RecentHashtagPack::const_iterator i = search.cbegin(); i != search.cend(); ++i) {
				if (!i->first.isEmpty()) {
					size += _stringSize(i->first) + sizeof(quint16);
					++searchCnt;
				}
			}
			EncryptedDescriptor data(size);
			data.stream << quint32(writeCnt) << quint32(searchCnt);
			for (RecentHashtagPack::const_iterator i = write.cbegin(); i != write.cend(); ++i) {
				if (!i->first.isEmpty()) data.stream << i->first << quint16(i->second);
			}
			for (RecentHashtagPack::const_iterator i = search.cbegin(); i != search.cend(); ++i) {
				if (!i->first.isEmpty()) data.stream << i->first << quint16(i->second);
			}
			FileWriteDescriptor file(_recentHashtagsKey);
			file.writeEncrypted(data);
		}
	}

	void readRecentHashtags() {
		if (_recentHashtagsWereRead) return;
		_recentHashtagsWereRead = true;

		if (!_recentHashtagsKey) return;

		FileReadDescriptor hashtags;
		if (!readEncryptedFile(hashtags, _recentHashtagsKey)) {
			clearKey(_recentHashtagsKey);
			_recentHashtagsKey = 0;
			_writeMap();
			return;
		}

		quint32 writeCount = 0, searchCount = 0;
		hashtags.stream >> writeCount >> searchCount;

		QString tag;
		quint16 count;
			
		RecentHashtagPack write, search;
		if (writeCount) {
			write.reserve(writeCount);
			for (uint32 i = 0; i < writeCount; ++i) {
				hashtags.stream >> tag >> count;
				write.push_back(qMakePair(tag.trimmed(), count));
			}
		}
		if (searchCount) {
			search.reserve(searchCount);
			for (uint32 i = 0; i < searchCount; ++i) {
				hashtags.stream >> tag >> count;
				search.push_back(qMakePair(tag.trimmed(), count));
			}
		}
		cSetRecentWriteHashtags(write);
		cSetRecentSearchHashtags(search);
	}

	struct ClearManagerData {
		QThread *thread;
		StorageMap images, stickers, audios;
		QMutex mutex;
		QList<int> tasks;
		bool working;
	};

	ClearManager::ClearManager() : data(new ClearManagerData()) {
		data->thread = new QThread();
		data->working = true;
	}

	bool ClearManager::addTask(int task) {
		QMutexLocker lock(&data->mutex);
		if (!data->working) return false;

		if (!data->tasks.isEmpty() && (data->tasks.at(0) == ClearManagerAll)) return true;
		if (task == ClearManagerAll) {
			data->tasks.clear();
			if (!_imagesMap.isEmpty()) {
				_imagesMap.clear();
				_storageImagesSize = 0;
				_mapChanged = true;
			}
			if (!_stickerImagesMap.isEmpty()) {
				_stickerImagesMap.clear();
				_storageStickersSize = 0;
				_mapChanged = true;
			}
			if (!_audiosMap.isEmpty()) {
				_audiosMap.clear();
				_storageAudiosSize = 0;
				_mapChanged = true;
			}
			if (!_draftsMap.isEmpty()) {
				_draftsMap.clear();
				_mapChanged = true;
			}
			if (!_draftsPositionsMap.isEmpty()) {
				_draftsPositionsMap.clear();
				_mapChanged = true;
			}
			if (_locationsKey) {
				_locationsKey = 0;
				_mapChanged = true;
			}
			if (_recentStickersKeyOld) {
				_recentStickersKeyOld = 0;
				_mapChanged = true;
			}
			if (_stickersKey) {
				_stickersKey = 0;
				_mapChanged = true;
			}
			if (_recentHashtagsKey) {
				_recentHashtagsKey = 0;
				_mapChanged = true;
			}
			_writeMap();
		} else {
			if (task & ClearManagerStorage) {
				if (data->images.isEmpty()) {
					data->images = _imagesMap;
				} else {
					for (StorageMap::const_iterator i = _imagesMap.cbegin(), e = _imagesMap.cend(); i != e; ++i) {
						StorageKey k = i.key();
						while (data->images.constFind(k) != data->images.cend()) {
							++k.second;
						}
						data->images.insert(k, i.value());
					}
				}
				if (!_imagesMap.isEmpty()) {
					_imagesMap.clear();
					_storageImagesSize = 0;
					_mapChanged = true;
				}
				if (data->stickers.isEmpty()) {
					data->stickers = _stickerImagesMap;
				} else {
					for (StorageMap::const_iterator i = _stickerImagesMap.cbegin(), e = _stickerImagesMap.cend(); i != e; ++i) {
						StorageKey k = i.key();
						while (data->stickers.constFind(k) != data->stickers.cend()) {
							++k.second;
						}
						data->stickers.insert(k, i.value());
					}
				}
				if (!_stickerImagesMap.isEmpty()) {
					_stickerImagesMap.clear();
					_storageStickersSize = 0;
					_mapChanged = true;
				}
				if (data->audios.isEmpty()) {
					data->audios = _audiosMap;
				} else {
					for (StorageMap::const_iterator i = _audiosMap.cbegin(), e = _audiosMap.cend(); i != e; ++i) {
						StorageKey k = i.key();
						while (data->audios.constFind(k) != data->audios.cend()) {
							++k.second;
						}
						data->audios.insert(k, i.value());
					}
				}
				if (!_audiosMap.isEmpty()) {
					_audiosMap.clear();
					_storageAudiosSize = 0;
					_mapChanged = true;
				}
				_writeMap();
			}
			for (int32 i = 0, l = data->tasks.size(); i < l; ++i) {
				if (data->tasks.at(i) == task) return true;
			}
		}
		data->tasks.push_back(task);
		return true;
	}

	bool ClearManager::hasTask(ClearManagerTask task) {
		QMutexLocker lock(&data->mutex);
		if (data->tasks.isEmpty()) return false;
		if (data->tasks.at(0) == ClearManagerAll) return true;
		for (int32 i = 0, l = data->tasks.size(); i < l; ++i) {
			if (data->tasks.at(i) == task) return true;
		}
		return false;
	}

	void ClearManager::start() {
		moveToThread(data->thread);
		connect(data->thread, SIGNAL(started()), this, SLOT(onStart()));
		data->thread->start();
	}

	ClearManager::~ClearManager() {
		data->thread->deleteLater();
		delete data;
	}

	void ClearManager::onStart() {
		while (true) {
			int task = 0;
			bool result = false;
			StorageMap images, stickers, audios;
			{
				QMutexLocker lock(&data->mutex);
				if (data->tasks.isEmpty()) {
					data->working = false;
					break;
				}
				task = data->tasks.at(0);
				images = data->images;
				stickers = data->stickers;
				audios = data->audios;
			}
			switch (task) {
			case ClearManagerAll: {
				result = QDir(cTempDir()).removeRecursively();
				QDirIterator di(_userBasePath, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
				while (di.hasNext()) {
					di.next();
					const QFileInfo& fi = di.fileInfo();
					if (fi.isDir() && !fi.isSymLink()) {
						if (!QDir(di.filePath()).removeRecursively()) result = false;
					} else {
						QString path = di.filePath();
						if (!path.endsWith(QLatin1String("map0")) && !path.endsWith(QLatin1String("map1"))) {
							if (!QFile::remove(di.filePath())) result = false;
						}
					}
				}
			} break;
			case ClearManagerDownloads:
				result = QDir(cTempDir()).removeRecursively();
			break;
			case ClearManagerStorage:
				for (StorageMap::const_iterator i = images.cbegin(), e = images.cend(); i != e; ++i) {
					clearKey(i.value().first, UserPath);
				}
				for (StorageMap::const_iterator i = stickers.cbegin(), e = stickers.cend(); i != e; ++i) {
					clearKey(i.value().first, UserPath);
				}
				for (StorageMap::const_iterator i = audios.cbegin(), e = audios.cend(); i != e; ++i) {
					clearKey(i.value().first, UserPath);
				}
				result = true;
			break;
			}
			{
				QMutexLocker lock(&data->mutex);
				if (data->tasks.at(0) == task) {
					data->tasks.pop_front();
					if (data->tasks.isEmpty()) {
						data->working = false;
					}
				}
				if (result) {
					emit succeed(task, data->working ? 0 : this);
				} else {
					emit failed(task, data->working ? 0 : this);
				}
				if (!data->working) break;
			}
		}
	}

}
