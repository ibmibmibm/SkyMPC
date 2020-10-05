#include "MusicPlayerClient.h"
#include <QHostAddress>


void Host::set(const QString &hostname, int port)
{
	QString address;
	int i = hostname.indexOf(':');
	if (i < 0) {
		address = hostname;
	} else {
		address = hostname.mid(0, i);
		port = hostname.mid(i + 1).toInt();
	}
	if (port < 1 || port > 65535) {
		port = 0;
	}
	data.address = address;
	data.port = port;
}

//

MusicPlayerClient::MusicPlayerClient()
{
}

MusicPlayerClient::~MusicPlayerClient()
{
	close();
}

bool MusicPlayerClient::isValidPlaylistName(const QString &name)
{
	if (name.isEmpty()) return false;
	ushort const *p = (ushort const *)name.data();
	while (*p) {
		if (*p < 0x20) return false;
		if (*p < 0x80 && strchr("\"\\/?|<>", *p)) return false;
		p++;
	}
	return true;
}

bool MusicPlayerClient::recv(QTcpSocket *sock, QStringList *lines)
{
	int timeout = 10000;
	while (sock->waitForReadyRead(timeout)) {
		while (sock->canReadLine()) {
			QByteArray ba = sock->readLine();
			QString s;
			int n = ba.size();
			if (n > 0) {
				char const *p = ba.data();
				if (n > 0 && p[n - 1] == '\n') {
					n--;
				}
				if (n > 0) {
					s = QString::fromUtf8(p, n);
				}
			}
			if (s == "OK") {
				return true;
			}
			lines->push_back(s);
			if (s.startsWith("ACK")) {
				int i = s.indexOf('}');
				if (i > 0) {
					ushort const *p = s.utf16();
					do {
						i++;
					} while (QChar(p[i]).isSpace());
					exception = s.mid(i);
				}
				return false;
			}
		}
		timeout = 1000;
	}
	return false;
}

bool MusicPlayerClient::exec(QString const &command, QStringList *lines)
{
	lines->clear();
	exception.clear();

	if (sock().waitForReadyRead(0)) {
		sock().readAll();
	}

	QByteArray ba = (command + '\n').toUtf8();
	sock().write(ba.data(), ba.size());

	return recv(&sock(), lines);
}

MusicPlayerClient::OpenResult MusicPlayerClient::open(QTcpSocket *sock, Host const &host, Logger *logger)
{
	OpenResult result;
	try {
		if (!host.isValid()) {
			throw QString("Specified host is not valid.");
		}
		if (logger) logger->append(tr("Attempting to connect to: ") + host.address() + "\n");
		sock->connectToHost(host.address(), host.port(DEFAULT_MPD_PORT));
		if (!sock->waitForConnected(10000)) throw sock->errorString();
		QString s = sock->peerAddress().toString();
		if (logger) logger->append(tr("Connected to: ") + s + '\n');
		if (!sock->waitForReadyRead(10000))  throw sock->errorString();

		if (logger) logger->append("---\n");

		QString text;
		if (sock->canReadLine()) {
			QByteArray ba = sock->readLine();
			if (!ba.isEmpty()) {
				text = QString::fromUtf8(ba.data(), ba.size());
			}
		}
		if (logger) logger->append(text);
		if (text.isEmpty()) throw QString("The server does not respond.");
		if (!text.startsWith("OK MPD ")) throw QString("Host is not MPD server.");

		result.success = true;
		QString pw = host.password();
		if (!pw.isEmpty()) {
			QString str = "password " + pw + '\n';
			sock->write(str.toUtf8());
			QStringList lines;
			if (recv(sock, &lines)) {
				result.success = true;
				result.incorrect_password = false;
			} else {
				for (QString const &line : lines) {
					QString msg = line + '\n';
					if (logger) logger->append(msg);
				}
				result.incorrect_password = true;
			}
		}
		if (logger) logger->append("\n---\n");
	} catch (...) {
		sock->close();
		throw;
	}
	return result;
}

bool MusicPlayerClient::open(Host const &host)
{
	try {
		OpenResult r = open(&sock(), host);
		if (r.success) {
			if (r.incorrect_password) {
				throw QString("Authentication failure.");
			}
			if (!ping()) throw QString("The server does not respond.");
			return true;
		}
		return false;
	} catch (QString const &e) {
		exception = e;
	}
	return false;
}

void MusicPlayerClient::close()
{
	if (isOpen()) {
		sock().write("close");
		sock().waitForBytesWritten(100);
		sock().close();
	}
}

bool MusicPlayerClient::isOpen() const
{
	QTcpSocket const *s = sock_p();
	return s && s->isOpen();
}

bool MusicPlayerClient::ping(int retry)
{
	for (int i = 0; i < retry; i++) {
		QStringList lines;
		if (exec("ping", &lines)) {
			return true;
		}
	}
	return false;
}

void MusicPlayerClient::parse_result(QStringList const &lines, QList<Item> *out)
{
	Item info;
	auto it = lines.begin();
	while (1) {
		QString key;
		QString value;
		bool end = false;
		if (it == lines.end()) {
			end = true;
		} else {
			QString line = *it;
			int i = line.indexOf(':');
			if (i > 0) {
				key = line.mid(0, i);
				value = line.mid(i + 1).trimmed();
			}
			it++;
		}
		bool low = QChar(key.utf16()[0]).isLower();
		if (low || end) {
			if (!info.kind.isEmpty() || !info.map.empty()) {
				out->push_back(info);
			}
			if (end) {
				break;
			}
			info = Item();
		}
		if (!key.isEmpty()) {
			if (low) {
				info.kind = key;
				info.text = value;
			} else {
				info.map.map[key] = value;
			}
		}
	}
}

void MusicPlayerClient::parse_result(QStringList const &lines, std::vector<KeyValue> *out)
{
	out->clear();
	auto it = lines.begin();
	while (1) {
		QString key;
		QString value;
		if (it == lines.end()) {
			return;
		}
		QString line = *it;
		int i = line.indexOf(':');
		if (i > 0) {
			key = line.mid(0, i);
			value = line.mid(i + 1).trimmed();
		}
		it++;
		if (!key.isEmpty()) {
			out->push_back(KeyValue(key, value));
		}
	}
}

void MusicPlayerClient::parse_result(QStringList const &lines, StringMap *out)
{
	out->clear();
	std::vector<KeyValue> vec;
	parse_result(lines, &vec);
	for (KeyValue const &item : vec) {
		out->map[item.key] = item.value;
	}
}

bool MusicPlayerClient::do_status(StringMap *out)
{
	out->map.clear();
	QStringList lines;
	if (exec("status", &lines)) {
		parse_result(lines, out);
		return true;
	}
	return false;
}

int MusicPlayerClient::get_volume()
{
	MusicPlayerClient::StringMap status;
	if (do_status(&status)) {
		return status.get("volume").toInt();
	}
	return -1;
}

void MusicPlayerClient::sort(QList<MusicPlayerClient::Item> *vec)
{
	std::sort(vec->begin(), vec->end(), [](MusicPlayerClient::Item const &left, MusicPlayerClient::Item const &right){
		int i;
		i = QString::compare(left.kind, right.kind, Qt::CaseInsensitive);
		if (i == 0) {
			i = QString::compare(left.text, right.text, Qt::CaseInsensitive);
			if (i == 0) {
				QString l_title = left.map.get("Title");
				QString r_title = right.map.get("Title");
				i = QString::compare(l_title, r_title, Qt::CaseInsensitive);
			}
		}
		return i < 0;
	});
}

QString MusicPlayerClient::timeText(const MusicPlayerClient::Item &item)
{
	unsigned int sec = item.map.get("Time").toUInt();
	if (sec > 0) {
		unsigned int m = sec / 60;
		unsigned int s = sec % 60;
		char tmp[100];
		sprintf(tmp, "%u:%02u", m, s);
		return tmp;
	}
	return QString();
}

template <typename T> bool MusicPlayerClient::info_(QString const &command, QString const &path, T *out)
{
	QStringList lines;
	QString cmd = command;
	if (!path.isEmpty()) {
		cmd = command + " \"" + path + '\"';
	}
	out->clear();
	if (exec(cmd, &lines)) {
		parse_result(lines, out);
		return true;
	}
	return false;
}

bool MusicPlayerClient::do_lsinfo(QString const &path, QList<Item> *out)
{
	return info_("lsinfo", path, out);
}

bool MusicPlayerClient::do_listall(QString const &path, QList<Item> *out)
{
	return info_("listall", path, out);
}

bool MusicPlayerClient::do_listfiles(QString const &path, QList<Item> *out)
{
	return info_("listfiles", path, out);
}

bool MusicPlayerClient::do_listallinfo(QString const &path, std::vector<KeyValue> *out)
{
	return info_("listallinfo", path, out);
}

bool MusicPlayerClient::do_listallinfo(QString const &path, QList<Item> *out)
{
	return info_("listallinfo", path, out);
}

bool MusicPlayerClient::do_clear()
{
	QStringList lines;
	return exec("clear", &lines);
}

bool MusicPlayerClient::do_playlist(QList<Item> *out)
{
	return info_("playlist", QString(), out);
}

bool MusicPlayerClient::do_playlistinfo(QString const &path, QList<Item> *out)
{
	if (info_("playlistinfo", path, out)) {
		int song_id = 0;
		for (int i = 0; i < out->size(); i++) {
			if ((*out)[i].kind == "file") {
				if ((*out)[i].map.map.find("Id") == (*out)[i].map.map.end()) { // for compatibility
					(*out)[i].map.map["Id"] = QString::number(song_id);
				}
				song_id++;
			}
		}
		return true;
	}
	return false;
}

bool MusicPlayerClient::do_add(QString const &path)
{
	QStringList lines;
	return exec(QString("add \"") + path + "\"", &lines);
}

bool MusicPlayerClient::do_deleteid(int id)
{
	QStringList lines;
	return exec(QString("delete ") + QString::number(id), &lines);
}

bool MusicPlayerClient::do_move(int from, int to)
{
	QStringList lines;
	return exec(QString("move ") + QString::number(from) + ' ' + QString::number(to), &lines);
}

bool MusicPlayerClient::do_swap(int a, int b)
{
	QStringList lines;
	return exec(QString("swap ") + QString::number(a) + ' ' + QString::number(b), &lines);
}

int MusicPlayerClient::do_addid(QString const &path, int to)
{
	QStringList lines;
	QString cmd = QString("addid \"") + path + '\"';
	if (to >= 0) {
		cmd += ' ';
		cmd += QString::number(to);
	}
	if (exec(cmd, &lines)) {
		StringMap map;
		parse_result(lines, &map);
		QString s = map.get("Id");
		if (!s.isEmpty()) {
			bool ok = false;
			int id = s.toInt(&ok);
			if (ok) {
				return id;
			}
		}
	}
	return -1;
}

bool MusicPlayerClient::do_currentsong(StringMap *out)
{
	QStringList lines;
	if (exec("currentsong", &lines)) {
		parse_result(lines, out);
		return true;
	}
	return false;
}

bool MusicPlayerClient::do_play(int i)
{
	QStringList lines;
	QString cmd = "play";
	if (i >= 0) {
		cmd += ' ';
		cmd += QString::number(i);
	}
	return exec(cmd, &lines);
}

bool MusicPlayerClient::do_pause(bool f)
{
	QStringList lines;
	return exec(f ? "pause 1" : "pause 0", &lines);
}

bool MusicPlayerClient::do_stop()
{
	QStringList lines;
	return exec("stop", &lines);
}

bool MusicPlayerClient::do_next()
{
	QStringList lines;
	return exec("next", &lines);
}

bool MusicPlayerClient::do_previous()
{
	QStringList lines;
	return exec("previous", &lines);
}

bool MusicPlayerClient::do_repeat(bool f)
{
	QStringList lines;
	return exec(f ? "repeat 1" : "repeat 0", &lines);
}

bool MusicPlayerClient::do_single(bool f)
{
	QStringList lines;
	return exec(f ? "single 1" : "single 0", &lines);
}

bool MusicPlayerClient::do_consume(bool f)
{
	QStringList lines;
	return exec(f ? "consume 1" : "consume 0", &lines);
}

bool MusicPlayerClient::do_random(bool f)
{
	QStringList lines;
	return exec(f ? "random 1" : "random 0", &lines);
}

bool MusicPlayerClient::do_setvol(int n)
{
	QStringList lines;
	return exec("setvol " + QString::number(n), &lines);
}

bool MusicPlayerClient::do_seek(int song, int pos)
{
	QStringList lines;
	return exec("seek " + QString::number(song) + ' ' + QString::number(pos), &lines);
}

bool MusicPlayerClient::do_save(QString const &name)
{
	QStringList lines;
	return exec(QString("save \"") + name + "\"", &lines);
}

bool MusicPlayerClient::do_load(QString const &name, const QString &range)
{
	QStringList lines;
	QString cmd = QString("load \"") + name + "\"";
	if (!range.isEmpty()) {
		cmd += ' ';
		cmd += range;
	}
	return exec(cmd, &lines);
}

bool MusicPlayerClient::do_listplaylistinfo(QString const &name, QList<Item> *out)
{
	return info_("listplaylistinfo", name, out);
}

bool MusicPlayerClient::do_rename(QString const &curname, QString const &newname)
{
	QStringList lines;
	return exec(QString("rename \"") + curname + "\" \"" + newname + "\"", &lines);
}

bool MusicPlayerClient::do_rm(QString const &name)
{
	QStringList lines;
	return exec(QString("rm \"") + name + "\"", &lines);
}

bool MusicPlayerClient::do_update()
{
	QStringList lines;
	return exec("update", &lines);
}

int MusicPlayerClient::current_playlist_file_count()
{
	QStringList lines;
	if (exec("playlist", &lines)) {
		return lines.size();
	}
	return 0;
}
