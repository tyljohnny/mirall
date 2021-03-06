/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "owncloudpropagator.h"
#include "syncjournaldb.h"
#include "syncjournalfilerecord.h"
#include <httpbf.h>
#include <qfile.h>
#include <qdir.h>
#include <qdiriterator.h>
#include <qtemporaryfile.h>
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
#include <qabstractfileengine.h>
#else
#include <qsavefile.h>
#endif
#include <QDebug>
#include <QDateTime>
#include <qstack.h>

#include <neon/ne_basic.h>
#include <neon/ne_socket.h>
#include <neon/ne_session.h>
#include <neon/ne_props.h>
#include <neon/ne_auth.h>
#include <neon/ne_dates.h>
#include <neon/ne_compress.h>
#include <neon/ne_redirect.h>

#include <time.h>

// We use some internals of csync:
extern "C" int c_utimes(const char *, const struct timeval *);
extern "C" void csync_win32_set_file_hidden( const char *file, bool h );

namespace Mirall {

/* Helper for QScopedPointer<>, to be used as the deleter.
 * QScopePointer will call the right overload of cleanup for the pointer it holds
 */
struct ScopedPointerHelpers {
    static inline void cleanup(hbf_transfer_t *pointer) { if (pointer) hbf_free_transfer(pointer); }
    static inline void cleanup(ne_request *pointer) { if (pointer) ne_request_destroy(pointer); }
    static inline void cleanup(ne_decompress *pointer) { if (pointer) ne_decompress_destroy(pointer); }
//     static inline void cleanup(ne_propfind_handler *pointer) { if (pointer) ne_propfind_destroy(pointer); }
};

#define DECLARE_JOB(NAME) \
class NAME : public PropagateItemJob { \
  /*  Q_OBJECT */ \
public: \
    NAME(OwncloudPropagator* propagator,const SyncFileItem& item) \
        : PropagateItemJob(propagator, item) {} \
    void start(); \
};

// compare two files with given filename and return true if they have the same content
static bool fileEquals(const QString &fn1, const QString &fn2) {
    QFile f1(fn1);
    QFile f2(fn2);
    if (!f1.open(QIODevice::ReadOnly) || !f2.open(QIODevice::ReadOnly)) {
        qDebug() << "fileEquals: Failed to open " << fn1 << "or" << fn2;
        return false;
    }

    if (f1.size() != f2.size()) {
        return false;
    }

    const int BufferSize = 16 * 1024;
    char buffer1[BufferSize];
    char buffer2[BufferSize];
    do {
        int r = f1.read(buffer1, BufferSize);
        if (f2.read(buffer2, BufferSize) != r) {
            // this should normaly not happen: the file are supposed to have the same size.
            return false;
        }
        if (r <= 0) {
            return true;
        }
        if (memcmp(buffer1, buffer2, r) != 0) {
            return false;
        }
    } while (true);
    return false;
}

// Code copied from Qt5's QDir::removeRecursively
static bool removeRecursively(const QString &path)
{
    bool success = true;
    QDirIterator di(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (di.hasNext()) {
        di.next();
        const QFileInfo& fi = di.fileInfo();
        bool ok;
        if (fi.isDir() && !fi.isSymLink())
            ok = removeRecursively(di.filePath()); // recursive
        else
            ok = QFile::remove(di.filePath());
        if (!ok)
            success = false;
    }
    if (success)
        success = QDir().rmdir(path);
    return success;
}

DECLARE_JOB(PropagateLocalRemove)

void PropagateLocalRemove::start()
{
    QString filename = _propagator->_localDir +  _item._file;
    if (_item._isDirectory) {
        if (QDir(filename).exists() && !removeRecursively(filename)) {
            done(SyncFileItem::NormalError, tr("Could not remove directory %1").arg(filename));
            return;
        }
    } else {
        QFile file(filename);
        if (file.exists() && !file.remove()) {
            done(SyncFileItem::NormalError, file.errorString());
        }
    }
    _propagator->_journal->deleteFileRecord(_item._originalFile);
    done(SyncFileItem::Success);
}

DECLARE_JOB(PropagateLocalMkdir)

void PropagateLocalMkdir::start()
{
    QDir d;
    if (!d.mkpath(_propagator->_localDir +  _item._file)) {
        done(SyncFileItem::NormalError, tr("could not create directory %1").arg(_propagator->_localDir +  _item._file));
        return;
    }
    done(SyncFileItem::Success);
}

DECLARE_JOB(PropagateRemoteRemove)

void PropagateRemoteRemove::start()
{
    QScopedPointer<char, QScopedPointerPodDeleter> uri(
        ne_path_escape((_propagator->_remoteDir + _item._file).toUtf8()));
    qDebug() << "** DELETE " << uri.data();
    int rc = ne_delete(_propagator->_session, uri.data());
    /* Ignore the error 404,  it means it is already deleted */
    if (updateErrorFromSession(rc, 0, 404)) {
        return;
    }
    _propagator->_journal->deleteFileRecord(_item._originalFile, _item._isDirectory);
    done(SyncFileItem::Success);
}

DECLARE_JOB(PropagateRemoteMkdir)

void PropagateRemoteMkdir::start()
{
    QScopedPointer<char, QScopedPointerPodDeleter> uri(
        ne_path_escape((_propagator->_remoteDir + _item._file).toUtf8()));

    int rc = ne_mkcol(_propagator->_session, uri.data());

    /* Special for mkcol: it returns 405 if the directory already exists.
     * Ignore that error */
    if( updateErrorFromSession( rc , 0, 405 ) ) {
        return;
    }
    done(SyncFileItem::Success);
}

class PropagateUploadFile: public PropagateItemJob {
public:
    explicit PropagateUploadFile(OwncloudPropagator* propagator,const SyncFileItem& item)
        : PropagateItemJob(propagator, item) {}
    void start();
private:
    // Log callback for httpbf
    static void _log_callback(const char *func, const char *text, void*)
    {
        qDebug() << "  " << func << text;
    }

    // abort callback for httpbf
    static int _user_want_abort(void *userData)
    {
        return  static_cast<PropagateUploadFile *>(userData)->_propagator->_abortRequested->fetchAndAddRelaxed(0);
    }

    // callback from httpbf when a chunk is finished
    static void chunk_finished_cb(hbf_transfer_s *trans, int chunk, void* userdata)
    {
        PropagateUploadFile *that = static_cast<PropagateUploadFile *>(userdata);
        Q_ASSERT(that);
        that->_chunked_done += trans->block_arr[chunk]->size;
        if (trans->block_cnt > 1) {
            SyncJournalDb::UploadInfo pi;
            pi._valid = true;
            pi._chunk = chunk + 1; // next chunk to start with
            pi._transferid = trans->transfer_id;
            pi._modtime =  QDateTime::fromTime_t(trans->modtime);
            that->_propagator->_journal->setUploadInfo(that->_item._file, pi);
        }
    }

    static void notify_status_cb(void* userdata, ne_session_status status,
                          const ne_session_status_info* info)
    {
        PropagateUploadFile* that = reinterpret_cast<PropagateUploadFile*>(userdata);

        if (status == ne_status_sending && info->sr.total > 0) {
            emit that->progress(Progress::Context, that->_item._file ,
                                     that->_chunked_done + info->sr.progress,
                                     that->_chunked_total_size ? that->_chunked_total_size : info->sr.total );

            that->limitBandwidth(that->_chunked_done + info->sr.progress,  that->_propagator->_uploadLimit);
        }
    }

    qint64 _chunked_done; // amount of bytes already sent with the previous chunks
    qint64 _chunked_total_size; // total size of the whole file
};

void PropagateUploadFile::start()
{
    emit progress(Progress::StartUpload, _item._file, 0, _item._size);

    QFile file(_propagator->_localDir + _item._file);
    if (!file.open(QIODevice::ReadOnly)) {
        done(SyncFileItem::NormalError, file.errorString());
        return;
    }
    QScopedPointer<char, QScopedPointerPodDeleter> uri(
        ne_path_escape((_propagator->_remoteDir + _item._file).toUtf8()));

    int attempts = 0;

    /*
     * do ten tries to upload the file chunked. Check the file size and mtime
     * before submitting a chunk and after having submitted the last one.
     * If the file has changed, retry.
     */
    qDebug() << "** PUT request to" << uri.data();
    do {
        Hbf_State state = HBF_SUCCESS;
        QScopedPointer<hbf_transfer_t, ScopedPointerHelpers> trans(hbf_init_transfer(uri.data()));
        trans->user_data = this;
        hbf_set_log_callback(trans.data(), _log_callback);
        hbf_set_abort_callback(trans.data(), _user_want_abort);
        trans.data()->chunk_finished_cb = chunk_finished_cb;
        Q_ASSERT(trans);
        state = hbf_splitlist(trans.data(), file.handle());

        const SyncJournalDb::UploadInfo progressInfo = _propagator->_journal->getUploadInfo(_item._file);
        if (progressInfo._valid) {
            if (progressInfo._modtime.toTime_t() == _item._modtime) {
                trans->start_id = progressInfo._chunk;
                trans->transfer_id = progressInfo._transferid;
            }
        }

        ne_set_notifier(_propagator->_session, notify_status_cb, this);
        _lastTime.restart();
        _lastProgress = 0;
        _chunked_done = 0;
        _chunked_total_size = _item._size;

        if( state == HBF_SUCCESS ) {
            QByteArray previousEtag;
            if (!_item._etag.isEmpty() && _item._etag != "empty_etag") {
                // We add quotes because the owncloud server always add quotes around the etag, and
                //  csync_owncloud.c's owncloud_file_id always strip the quotes.
                previousEtag = '"' + _item._etag + '"';
                trans->previous_etag = previousEtag.data();
            }
            _chunked_total_size = trans->stat_size;
            qDebug() << "About to upload " << _item._file << "  (" << previousEtag << _item._size << " bytes )";
            /* Transfer all the chunks through the HTTP session using PUT. */
            state = hbf_transfer( _propagator->_session, trans.data(), "PUT" );
        }

        // the file id should only be empty for new files up- or downloaded
        QString fid = QString::fromUtf8( hbf_transfer_file_id( trans.data() ));
        if( _item._fileId.isEmpty() ) {
            if( fid.isEmpty() ) {
                const char *plain_uri = uri.data();
                getFileId(plain_uri);
            } else {
                _item._fileId = fid;
            }
        } else {
            if( _item._fileId != fid ) {
                qDebug() << "WARN: File ID changed!" << _item._fileId << fid;
            } else {
                qDebug() << "FileID remains" << _item._fileId;
            }
        }

        /* Handle errors. */
        if ( state != HBF_SUCCESS ) {

            /* If the source file changed during submission, lets try again */
            if( state == HBF_SOURCE_FILE_CHANGE ) {
              if( attempts++ < 30 ) { /* FIXME: How often do we want to try? */
                qDebug("SOURCE file has changed during upload, retry #%d in two seconds!", attempts);
                sleep(2);
                continue;
              }
            }
            // FIXME: find out the error class.
            //_httpStatusCode = hbf_fail_http_code(trans.data());
            done(SyncFileItem::NormalError, hbf_error_string(trans.data(), state));
            return;
        }

        ne_set_notifier(_propagator->_session, 0, 0);

        if( trans->modtime_accepted ) {
            _item._etag =  QByteArray(hbf_transfer_etag( trans.data() ));
        } else {
            updateMTimeAndETag(uri.data(), _item._modtime);
        }

        _propagator->_journal->setFileRecord(SyncJournalFileRecord(_item, _propagator->_localDir + _item._file));
        // Remove from the progress database:
        _propagator->_journal->setUploadInfo(_item._file, SyncJournalDb::UploadInfo());
        emit progress(Progress::EndUpload, _item._file, 0, _item._size);
        done(SyncFileItem::Success);
        return;

    } while( true );
}

static QByteArray parseEtag(ne_request *req) {
    const char *header = ne_get_response_header(req, "etag");
    if(header && header [0] == '"' && header[ strlen(header)-1] == '"') {
        return QByteArray(header + 1, strlen(header)-2);
    } else {
        return header;
    }
}

static QString parseFileId(ne_request *req) {
    QString fileId;

    const char *header = ne_get_response_header(req, "OC-FileId");
    if( header ) {
        fileId = QString::fromUtf8(header);
    }
    return fileId;
}

void PropagateItemJob::updateMTimeAndETag(const char* uri, time_t mtime)
{
    QByteArray modtime = QByteArray::number(qlonglong(mtime));
    ne_propname pname;
    pname.nspace = "DAV:";
    pname.name = "lastmodified";
    ne_proppatch_operation ops[2];
    ops[0].name = &pname;
    ops[0].type = ne_propset;
    ops[0].value = modtime.constData();
    ops[1].name = NULL;

    int rc = ne_proppatch( _propagator->_session, uri, ops );
    /* FIXME: error handling
    bool error = updateErrorFromSession( rc );
    if( error ) {
        // FIXME: We could not set the mtime. Error or not?
        qDebug() << "PROP-Patching of modified date failed.";
    }*/

    // get the etag
    QScopedPointer<ne_request, ScopedPointerHelpers> req(ne_request_create(_propagator->_session, "HEAD", uri));
    int neon_stat = ne_request_dispatch(req.data());
    const ne_status *status = ne_get_status(req.data());
    if( neon_stat != NE_OK || status->klass != 2 ) {
        // error happend
        qDebug() << "Could not issue HEAD request for ETag." << ne_get_error(_propagator->_session);
    } else {
        _item._etag = parseEtag(req.data());
        QString fid = parseFileId(req.data());
        if( _item._fileId.isEmpty() ) {
            _item._fileId = fid;
            qDebug() << "FileID was empty, set it to " << _item._fileId;
        } else {
            if( !fid.isEmpty() && fid != _item._fileId ) {
                qDebug() << "WARN: FileID seems to have changed: "<< fid << _item._fileId;
            } else {
                qDebug() << "FileID is " << _item._fileId;
            }
        }
    }
}

void PropagateItemJob::getFileId(const char* uri)
{
    if( ! uri ) return;

    QScopedPointer<ne_request, ScopedPointerHelpers> req(ne_request_create(_propagator->_session, "HEAD", uri));
    qDebug() << "Querying the fileID from " << uri;
    int neon_stat = ne_request_dispatch(req.data());
    if( neon_stat != NE_OK ) {
        // error happend
        qDebug() << "Could not issue HEAD request for FileID.";
    } else {
        _item._fileId = parseFileId( req.data() );
    }
}

void PropagateItemJob::limitBandwidth(qint64 progress, qint64 bandwidth_limit)
{
    if (bandwidth_limit > 0) {
        int64_t diff = _lastTime.nsecsElapsed() / 1000;
        int64_t len = progress - _lastProgress;
        if (len > 0 && diff > 0 && (1000000 * len / diff) > bandwidth_limit) {
            int64_t wait_time = (1000000 * len / bandwidth_limit) - diff;
            if (wait_time > 0) {
                usleep(wait_time);
            }
        }
        _lastProgress = progress;
        _lastTime.start();
    } else if (bandwidth_limit < 0 && bandwidth_limit > -100) {
        int64_t diff = _lastTime.nsecsElapsed() / 1000;
        if (diff > 0) {
            // -bandwidth_limit is the % of bandwidth
            int64_t wait_time = -diff * (1 + 100.0 / bandwidth_limit);
            if (wait_time > 0) {
                usleep(wait_time);
            }
        }
        _lastTime.start();
    }
}

class PropagateDownloadFile: public PropagateItemJob {
public:
    explicit PropagateDownloadFile(OwncloudPropagator* propagator,const SyncFileItem& item)
        : PropagateItemJob(propagator, item), _file(0) {}
    void start();

private:
    QIODevice *_file;
    QScopedPointer<ne_decompress, ScopedPointerHelpers> _decompress;

    static int content_reader(void *userdata, const char *buf, size_t len)
    {
        PropagateDownloadFile *that = static_cast<PropagateDownloadFile *>(userdata);
        size_t written = 0;

        if (that->_propagator->_abortRequested->fetchAndAddRelaxed(0)) {
            ne_set_error(that->_propagator->_session, "Aborted by user");
            return NE_ERROR;
        }

        if(buf) {
            written = that->_file->write(buf, len);
            if( len != written ) {
                qDebug() << "WRN: content_reader wrote wrong num of bytes:" << len << "," << written;
            }
            return NE_OK;
        }
        return NE_ERROR;
    }

    /*
    * This hook is called after the response is here from the server, but before
    * the response body is parsed. It decides if the response is compressed and
    * if it is it installs the compression reader accordingly.
    * If the response is not compressed, the normal response body reader is installed.
    */
    static void install_content_reader( ne_request *req, void *userdata, const ne_status *status )
    {
        PropagateDownloadFile *that = static_cast<PropagateDownloadFile *>(userdata);

        Q_UNUSED(status);

        if( !that ) {
            qDebug("Error: install_content_reader called without valid write context!");
            return;
        }

        const char *enc = ne_get_response_header( req, "Content-Encoding" );
        qDebug("Content encoding ist <%s> with status %d", enc ? enc : "empty",
                    status ? status->code : -1 );

        if( enc == QLatin1String("gzip") ) {
            that->_decompress.reset(ne_decompress_reader( req, ne_accept_2xx,
                                                             content_reader,     /* reader callback */
                                                             that ));  /* userdata        */
        } else {
            ne_add_response_body_reader( req, ne_accept_2xx,
                                        content_reader,
                                        (void*) that );
        }
    }

    static void notify_status_cb(void* userdata, ne_session_status status,
                          const ne_session_status_info* info)
    {
        PropagateDownloadFile* that = reinterpret_cast<PropagateDownloadFile*>(userdata);
        if (status == ne_status_recving && info->sr.total > 0) {
            emit that->progress(Progress::Context, that->_item._file, info->sr.progress, info->sr.total );
            that->limitBandwidth(info->sr.progress,  that->_propagator->_downloadLimit);
        }
    }
};

void PropagateDownloadFile::start()
{
    emit progress(Progress::StartDownload, _item._file, 0, _item._size);

    QString tmpFileName;
    const SyncJournalDb::DownloadInfo progressInfo = _propagator->_journal->getDownloadInfo(_item._file);
    if (progressInfo._valid) {
        // if the etag has changed meanwhile, remove the already downloaded part.
        if (progressInfo._etag != _item._etag) {
            QFile::remove(_propagator->_localDir + progressInfo._tmpfile);
            _propagator->_journal->setDownloadInfo(_item._file, SyncJournalDb::DownloadInfo());
        } else {
            tmpFileName = progressInfo._tmpfile;
        }

    }

    if (tmpFileName.isEmpty()) {
        tmpFileName = _item._file;
        //add a dot at the begining of the filename to hide the file.
        int slashPos = tmpFileName.lastIndexOf('/');
        tmpFileName.insert(slashPos+1, '.');
        //add the suffix
        tmpFileName += ".~" + QString::number(uint(qrand()), 16);
    }

    QFile tmpFile(_propagator->_localDir + tmpFileName);
    _file = &tmpFile;
    if (!tmpFile.open(QIODevice::Append)) {
        done(SyncFileItem::NormalError, tmpFile.errorString());
        return;
    }

    csync_win32_set_file_hidden(tmpFileName.toUtf8().constData(), true);

    {
        SyncJournalDb::DownloadInfo pi;
        pi._etag = _item._etag;
        pi._tmpfile = tmpFileName;
        pi._valid = true;
        _propagator->_journal->setDownloadInfo(_item._file, pi);
    }

    /* actually do the request */
    int retry = 0;

    QScopedPointer<char, QScopedPointerPodDeleter> uri(
        ne_path_escape((_propagator->_remoteDir + _item._file).toUtf8()));

    do {
        QScopedPointer<ne_request, ScopedPointerHelpers> req(ne_request_create(_propagator->_session, "GET", uri.data()));

        /* Allow compressed content by setting the header */
        ne_add_request_header( req.data(), "Accept-Encoding", "gzip" );

        if (tmpFile.size() > 0) {
            char brange[64];
            ne_snprintf(brange, sizeof brange, "bytes=%lld-", (long long) tmpFile.size());
            ne_add_request_header(req.data(), "Range", brange);
            ne_add_request_header(req.data(), "Accept-Ranges", "bytes");
            qDebug("Retry with range %s", brange);
        }

        /* hook called before the content is parsed to set the correct reader,
         * either the compressed- or uncompressed reader.
         */
        ne_hook_post_headers( _propagator->_session, install_content_reader, this);
        ne_set_notifier(_propagator->_session, notify_status_cb, this);
        _lastProgress = 0;
        _lastTime.start();

        int neon_stat = ne_request_dispatch(req.data());

        /* delete the hook again, otherwise they get chained as they are with the session */
        ne_unhook_post_headers( _propagator->_session, install_content_reader, this );
        ne_set_notifier(_propagator->_session, 0, 0);

        if (neon_stat == NE_TIMEOUT && (++retry) < 3) {
            continue;
        }

        if( updateErrorFromSession(neon_stat, req.data() ) ) {
            qDebug("Error GET: Neon: %d", neon_stat);
            if (tmpFile.size() == 0) {
                // don't keep the temporary file if it is empty.
                tmpFile.close();
                tmpFile.remove();
                _propagator->_journal->setDownloadInfo(_item._file, SyncJournalDb::DownloadInfo());
            }
            return;
        }
        _item._etag = parseEtag(req.data());
        break;
    } while (1);

    tmpFile.close();
    tmpFile.flush();
    QString fn = _propagator->_localDir + _item._file;


    bool isConflict = _item._instruction == CSYNC_INSTRUCTION_CONFLICT
            && !fileEquals(fn, tmpFile.fileName()); // compare the files to see if there was an actual conflict.
    //In case of conflict, make a backup of the old file
    if (isConflict) {
        QFile f(fn);
        // Add _conflict-XXXX  before the extention.
        int dotLocation = fn.lastIndexOf('.');
        // If no extention, add it at the end  (take care of cases like foo/.hidden or foo.bar/file)
        if (dotLocation <= fn.lastIndexOf('/') + 1) {
            dotLocation = fn.size();
        }
        fn.insert(dotLocation, "_conflict-" + QDateTime::fromTime_t(_item._modtime).toString("yyyyMMdd-hhmmss"));
        if (!f.rename(fn)) {
            //If the rename fails, don't replace it.
            done(SyncFileItem::NormalError, f.errorString());
            return;
        }
    }

    csync_win32_set_file_hidden(tmpFileName.toUtf8().constData(), false);

#ifndef QT_OS_WIN
    bool success;
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
    success = tmpFile.fileEngine()->rename(fn);
#else
    // We want a rename that also overwite.  QFile::rename does not overwite.
    // Qt 5.1 has QSaveFile::renameOverwrite we cold use.
    // ### FIXME
    QFile::remove(fn);
    success = tmpFile.rename(fn);
#endif
    // unixoids
    if (!success) {
        done(SyncFileItem::NormalError, tmpFile.errorString());
        return;
    }
#else //QT_OS_WIN
    if (::MoveFileEx((wchar_t*)tmpFile.fileName().utf16(),
                            (wchar_t*)QString(_localDir + item._file).utf16(),
                        MOVEFILE_REPLACE_EXISTING) != 0) {
        wchar_t *string = 0;
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
                      NULL, ::GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      (LPWSTR)&string, 0, NULL);

        done(SyncFileItem::NormalError, QString::fromWCharArray(string));
        LocalFree((HLOCAL)string);
        return;
    }
#endif
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = _item._modtime;
    times[0].tv_usec = times[1].tv_usec = 0;
    c_utimes(fn.toUtf8().data(), times);

    _propagator->_journal->setFileRecord(SyncJournalFileRecord(_item, fn));
    _propagator->_journal->setDownloadInfo(_item._file, SyncJournalDb::DownloadInfo());
    emit progress(Progress::EndDownload, _item._file, 0, _item._size);
    done(isConflict ? SyncFileItem::Conflict : SyncFileItem::Success);
}

DECLARE_JOB(PropagateLocalRename)

void PropagateLocalRename::start()
{
    if (_item._file != _item._renameTarget) {
        qDebug() << "MOVE " << _propagator->_localDir + _item._file << " => " << _propagator->_localDir + _item._renameTarget;
        QFile::rename(_propagator->_localDir + _item._file, _propagator->_localDir + _item._renameTarget);
    }

    _item._instruction = CSYNC_INSTRUCTION_DELETED;
    _propagator->_journal->deleteFileRecord(_item._originalFile);

    SyncJournalFileRecord record(_item, _propagator->_remoteDir + _item._file);
    record._path = _item._renameTarget;
    _propagator->_journal->setFileRecord(record);
    emit progress(Progress::EndDownload, _item._file, 0, _item._size);
    done(SyncFileItem::Success);
}

DECLARE_JOB(PropagateRemoteRename)

void PropagateRemoteRename::start()
{
    if (_item._file == _item._renameTarget) {
        if (!_item._isDirectory) {
            // The parents has been renamed already so there is nothing more to do.
            // But we still need to fetch the new ETAG
            // FIXME   maybe do a recusrsive propfind after having moved the parent.
            // Note: we also update the mtime because the server do not keep the mtime when moving files
            QScopedPointer<char, QScopedPointerPodDeleter> uri2(
                ne_path_escape((_propagator->_remoteDir + _item._renameTarget).toUtf8()));
            updateMTimeAndETag(uri2.data(), _item._modtime);
        }
    } else if (_item._file == QLatin1String("Shared") ) {
        // Check if it is the toplevel Shared folder and do not propagate it.
        if( QFile::rename(  _propagator->_localDir + _item._renameTarget, _propagator->_localDir + QLatin1String("Shared")) ) {
            done(SyncFileItem::NormalError, tr("This folder must not be renamed. It is renamed back to its original name."));
        } else {
            done(SyncFileItem::NormalError, tr("This folder must not be renamed. Please name it back to Shared."));
        }
        return;
    } else {

        QScopedPointer<char, QScopedPointerPodDeleter> uri1(ne_path_escape((_propagator->_remoteDir + _item._file).toUtf8()));
        QScopedPointer<char, QScopedPointerPodDeleter> uri2(ne_path_escape((_propagator->_remoteDir + _item._renameTarget).toUtf8()));

        int rc = ne_move(_propagator->_session, 1, uri1.data(), uri2.data());
        if (updateErrorFromSession(rc)) {
            return;
        }

        updateMTimeAndETag(uri2.data(), _item._modtime);
    }

    _propagator->_journal->deleteFileRecord(_item._originalFile);
    SyncJournalFileRecord record(_item, _propagator->_localDir + _item._renameTarget);
    record._path = _item._renameTarget;
    _propagator->_journal->setFileRecord(record);
    done(SyncFileItem::Success);
}

bool PropagateItemJob::updateErrorFromSession(int neon_code, ne_request* req, int ignoreHttpCode)
{
    if( neon_code != NE_OK ) {
        qDebug("Neon error code was %d", neon_code);
    }

    QString errorString;
    int httpStatusCode = 0;

    switch(neon_code) {
    case NE_OK:     /* Success, but still the possiblity of problems */
        if( req ) {
            const ne_status *status = ne_get_status(req);
            if (status) {
                if ( status->klass == 2 || status->code == ignoreHttpCode) {
                    // Everything is ok, no error.
                    return false;
                }
                errorString = QString::fromUtf8( status->reason_phrase );
                httpStatusCode = status->code;
            }
        } else {
            errorString = QString::fromUtf8(ne_get_error(_propagator->_session));
            httpStatusCode = errorString.mid(0, errorString.indexOf(QChar(' '))).toInt();
            if ((httpStatusCode >= 200 && httpStatusCode < 300)
                || (httpStatusCode != 0 && httpStatusCode == ignoreHttpCode)) {
                // No error
                return false;
            }
        }
        // FIXME: classify the error
        done (SyncFileItem::NormalError, errorString);
        return true;
    case NE_ERROR:  /* Generic error; use ne_get_error(session) for message */
        errorString = QString::fromUtf8(ne_get_error(_propagator->_session));
        if (ignoreHttpCode) {
            // Check if we don't need to ignore that error.
            httpStatusCode = errorString.mid(0, errorString.indexOf(QChar(' '))).toInt();
            if (httpStatusCode == ignoreHttpCode)
                return false;
        }
        done(SyncFileItem::NormalError, errorString);
        return true;
    case NE_LOOKUP:  /* Server or proxy hostname lookup failed */
    case NE_AUTH:     /* User authentication failed on server */
    case NE_PROXYAUTH:  /* User authentication failed on proxy */
    case NE_CONNECT:  /* Could not connect to server */
    case NE_TIMEOUT:  /* Connection timed out */
        done(SyncFileItem::FatalError, QString::fromUtf8(ne_get_error(_propagator->_session)));
        return true;
    case NE_FAILED:   /* The precondition failed */
    case NE_RETRY:    /* Retry request (ne_end_request ONLY) */
    case NE_REDIRECT: /* See ne_redirect.h */
    default:
        done(SyncFileItem::SoftError, QString::fromUtf8(ne_get_error(_propagator->_session)));
        return true;
    }
    return false;
}

PropagateItemJob* OwncloudPropagator::createJob(const SyncFileItem& item) {
    switch(item._instruction) {
        case CSYNC_INSTRUCTION_REMOVE:
            if (item._dir == SyncFileItem::Down) return new PropagateLocalRemove(this, item);
            else return new PropagateRemoteRemove(this, item);
        case CSYNC_INSTRUCTION_NEW:
            if (item._isDirectory) {
                if (item._dir == SyncFileItem::Down) return new PropagateLocalMkdir(this, item);
                else return new PropagateRemoteMkdir(this, item);
            }   //fall trough
        case CSYNC_INSTRUCTION_SYNC:
        case CSYNC_INSTRUCTION_CONFLICT:
            if (item._isDirectory) {
                // Should we set the mtime?
                return 0;
            }
            if (item._dir != SyncFileItem::Up) return new PropagateDownloadFile(this, item);
            else return new PropagateUploadFile(this, item);
        case CSYNC_INSTRUCTION_RENAME:
            if (item._dir == SyncFileItem::Up) {
                return new PropagateRemoteRename(this, item);
            } else {
                return new PropagateLocalRename(this, item);
            }
        case CSYNC_INSTRUCTION_IGNORE:
            return new PropagateIgnoreJob(this, item);
        default:
            return 0;
    }
    return 0;
}

void OwncloudPropagator::start(const SyncFileItemVector& _syncedItems)
{
    /* This builds all the job needed for the propagation.
     * Each directories is a PropagateDirectory job, which contains the files in it.
     * In order to do that we sort the items by destination. and loop over it. When we enter a
     * directory, we can create the directory job and push it on the stack. */
    SyncFileItemVector items = _syncedItems;
    std::sort(items.begin(), items.end());
    _rootJob.reset(new PropagateDirectory(this));
    QStack<QPair<QString /* directory name */, PropagateDirectory* /* job */> > directories;
    directories.push(qMakePair(QString(), _rootJob.data()));
    QVector<PropagatorJob*> directoriesToRemove;
    QString removedDirectory;
    foreach(const SyncFileItem &item, items) {
        if (item._instruction == CSYNC_INSTRUCTION_REMOVE
            && !removedDirectory.isEmpty() && item._file.startsWith(removedDirectory)) {
            //already taken care of.  (by the removal of the parent directory)
            continue;
        }

        while (!item._file.startsWith(directories.top().first)) {
            directories.pop();
        }

        if (item._isDirectory) {
            PropagateDirectory *dir = new PropagateDirectory(this, item);
            dir->_firstJob.reset(createJob(item));
            if (item._instruction == CSYNC_INSTRUCTION_REMOVE) {
                //We do the removal of directories at the end
                directoriesToRemove.append(dir);
                removedDirectory = item._file + "/";
            } else {
                directories.top().second->append(dir);
            }
            directories.push(qMakePair(item._file + "/" , dir));
        } else if (PropagateItemJob* current = createJob(item)) {
            directories.top().second->append(current);
        }
    }

    foreach(PropagatorJob* it, directoriesToRemove) {
        _rootJob->append(it);
    }

    connect(_rootJob.data(), SIGNAL(completed(SyncFileItem)), this, SIGNAL(completed(SyncFileItem)));
    connect(_rootJob.data(), SIGNAL(progress(Progress::Kind,QString,quint64,quint64)), this, SIGNAL(progress(Progress::Kind,QString,quint64,quint64)));
    connect(_rootJob.data(), SIGNAL(finished(SyncFileItem::Status)), this, SIGNAL(finished()));
    _rootJob->start();
}

void PropagateDirectory::proceedNext(SyncFileItem::Status status)
{
    if (status == SyncFileItem::FatalError) {
        emit finished(status);
        return;
    } else if (status == SyncFileItem::NormalError) {
        _hasError = true;
    }

    _current ++;
    if (_current < _subJobs.size()) {
        PropagatorJob *next = _subJobs.at(_current);
        startJob(next);
    } else {
        if (!_item.isEmpty() && !_hasError) {
            SyncJournalFileRecord record(_item,  _propagator->_localDir + _item._file);
            _propagator->_journal->setFileRecord(record);
        }
        emit finished(_hasError ? SyncFileItem::NormalError : SyncFileItem::Success);
    }
}

}
