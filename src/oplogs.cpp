/*
 * oplog.cpp
 *
 *  Created on: 2013-5-1
 *      Author: wqy
 */
#include "replication.hpp"
#include "ardb_server.hpp"
#include <sstream>

namespace ardb
{
	static const uint8 kSetOpType = 1;
	static const uint8 kDelOpType = 2;
	static const uint8 kOtherOpType = 3;

	static const uint32 k_max_rolling_index = 3;

	CachedWriteOp::CachedWriteOp(uint8 t, OpKey& k) :
			CachedOp(t), key(k)
	{
	}
	CachedCmdOp::CachedCmdOp(RedisCommandFrame* c) :
			CachedOp(kOtherOpType), cmd(c)
	{
	}
	CachedCmdOp::~CachedCmdOp()
	{
		DELETE(cmd);
	}

	OpKey::OpKey(const DBID& id, const std::string& k) :
			db(id), key(k)
	{

	}
	bool OpKey::operator<(const OpKey& other) const
	{
		if (db > other.db)
		{
			return false;
		}
		if (key == other.key)
		{
			return key < other.key;
		}
		return true;
	}

	OpLogs::OpLogs(ArdbServerConfig& cfg, Ardb* db) :
			m_cfg(cfg), m_db(db), m_min_seq(0), m_max_seq(0), m_op_log_file(
					NULL), m_current_oplog_record_size(0), m_last_flush_time(0)
	{

	}

	void OpLogs::LoadCachedOpLog(Buffer & buf)
	{
		uint32 mark = buf.GetReadIndex();
		while (true)
		{
			mark = buf.GetReadIndex();
			uint64 seq = 0;
			if (!BufferHelper::ReadVarUInt64(buf, seq))
			{
				break;
			}
			uint8 type;
			if (!BufferHelper::ReadFixUInt8(buf, type))
			{
				break;
			}
			if (type == kSetOpType || type == kDelOpType)
			{
				std::string key;
				if (!BufferHelper::ReadVarString(buf, key))
				{
					break;
				}
				OpKey ok(m_current_db, key);
				SaveWriteOp(ok, type, false);
			} else
			{
				uint32 size;
				if (!BufferHelper::ReadVarUInt32(buf, size))
				{
					break;
				}
				ArgumentArray strs;
				for (uint32 i = 0; i < size; i++)
				{
					std::string str;
					if (!BufferHelper::ReadVarString(buf, str))
					{
						goto ret;
					}
					strs.push_back(str);
				}
				RedisCommandFrame* cmd = new RedisCommandFrame(strs);
				if (!strcasecmp(cmd->GetCommand().c_str(), "select"))
				{
					m_current_db = *(cmd->GetArgument(1));
				}
				SaveCmdOp(cmd, false);
			}
			if (m_min_seq == 0)
			{
				m_min_seq = seq;
			}
			m_current_oplog_record_size++;
		}
		ret: buf.SetReadIndex(mark);
	}

	void OpLogs::LoadCachedOpLog(const std::string& file)
	{
		FILE* cacheOpFile = fopen(file.c_str(), "rb");
		if (NULL == cacheOpFile)
		{
			ERROR_LOG("Failed to open oplog file:%s", file.c_str());
			return;
		}
		Buffer buffer(1024 * 1024);
		while (true)
		{
			char* tmpbuf = const_cast<char*>(buffer.GetRawWriteBuffer());
			uint32 tmpbufsize = buffer.WriteableBytes();
			size_t ret = fread(tmpbuf, 1, tmpbufsize, cacheOpFile);
			buffer.AdvanceWriteIndex(ret);
			LoadCachedOpLog(buffer);
			buffer.DiscardReadedBytes();
			if (ret < tmpbufsize)
			{
				break;
			}
		}
		if (buffer.ReadableBytes() > 0)
		{
			ERROR_LOG("Corrupted oplog file.");
		}
		if (NULL != cacheOpFile)
		{
			fclose(cacheOpFile);
		}
	}

	void OpLogs::Load()
	{
		Buffer keybuf;
		std::string serverkey_path = m_cfg.repl_data_dir + "/repl.key";
		if (file_read_full(serverkey_path, keybuf) == 0)
		{
			m_server_key = keybuf.AsString();
		} else
		{
			m_server_key = random_string(16);
			file_write_content(serverkey_path, m_server_key);
		}
		INFO_LOG("Server replication key is %s", m_server_key.c_str());
		std::string filename = m_cfg.repl_data_dir + "/repl.oplog.1";
		if (is_file_exist(filename))
		{
			LoadCachedOpLog(filename);
		}
		m_current_oplog_record_size = 0;
		filename = m_cfg.repl_data_dir + "/repl.oplog";
		if (is_file_exist(filename))
		{
			LoadCachedOpLog(filename);
		}
		ReOpenOpLog();
	}

	void OpLogs::Routine()
	{
		time_t now = time(NULL);
		if (now - m_last_flush_time >= 5)
		{
			FlushOpLog();
		}
	}

	void OpLogs::ReOpenOpLog()
	{
		if (NULL == m_op_log_file)
		{
			std::string oplog_file_path = m_cfg.repl_data_dir + "/repl.oplog";
			m_op_log_file = fopen(oplog_file_path.c_str(), "a+");
			if (NULL == m_op_log_file)
			{
				ERROR_LOG(
						"Failed to open oplog file:%s", oplog_file_path.c_str());
			}
		}
	}

	void OpLogs::RollbackOpLogs()
	{
		if (NULL != m_op_log_file)
		{
			fclose(m_op_log_file);
			m_op_log_file = NULL;
		}
		std::string oplog_file_path = m_cfg.repl_data_dir + "/repl.oplog";
		std::stringstream oldest_file(
				std::stringstream::in | std::stringstream::out);
		oldest_file << oplog_file_path << "." << k_max_rolling_index;
		remove(oldest_file.str().c_str());

		for (int i = k_max_rolling_index - 1; i >= 1; --i)
		{
			std::stringstream source_oss(
					std::stringstream::in | std::stringstream::out);
			std::stringstream target_oss(
					std::stringstream::in | std::stringstream::out);

			source_oss << oplog_file_path << "." << i;
			target_oss << oplog_file_path << "." << (i + 1);
			if (is_file_exist(source_oss.str()))
			{
				remove(target_oss.str().c_str());
				rename(source_oss.str().c_str(), target_oss.str().c_str());
			}
			//loglog_renaming_result(*loglog, source, target, ret);
		}
		std::stringstream ss(std::stringstream::in | std::stringstream::out);
		ss << oplog_file_path << ".1";
		std::string path = ss.str();
		rename(oplog_file_path.c_str(), path.c_str());
		ReOpenOpLog();
	}

	void OpLogs::FlushOpLog()
	{
		if (m_op_log_buffer.Readable() && NULL != m_op_log_file)
		{
			fwrite(m_op_log_buffer.GetRawReadBuffer(), 1,
					m_op_log_buffer.ReadableBytes(), m_op_log_file);
			fflush(m_op_log_file);
			m_op_log_buffer.Clear();
			if (m_current_oplog_record_size >= m_cfg.rep_backlog_size)
			{
				//rollback op logs
				RollbackOpLogs();
				m_current_oplog_record_size = 0;
				ArgumentArray strs;
				strs.push_back("select");
				strs.push_back(m_current_db);
				SaveCmdOp(new RedisCommandFrame(strs));
			}
		}
	}

	void OpLogs::WriteCachedOp(uint64 seq, CachedOp* op)
	{
		Buffer tmp;
		BufferHelper::WriteVarUInt16(tmp, seq);
		uint8 optype = op->type;
		tmp.WriteByte(optype);
		if (optype == kSetOpType || optype == kDelOpType)
		{
			CachedWriteOp* writeOp = (CachedWriteOp*) op;
			BufferHelper::WriteVarString(tmp, writeOp->key.key);
		} else
		{
			CachedCmdOp* cmdOp = (CachedCmdOp*) op;
			BufferHelper::WriteVarUInt32(tmp,
					cmdOp->cmd->GetArguments().size() + 1);
			BufferHelper::WriteVarString(tmp, cmdOp->cmd->GetCommand());
			for (uint32 i = 0; i < cmdOp->cmd->GetArguments().size(); i++)
			{
				BufferHelper::WriteVarString(tmp,
						*(cmdOp->cmd->GetArgument(i)));
			}
		}
		m_op_log_buffer.Write(&tmp, tmp.ReadableBytes());
		m_current_oplog_record_size++;
		if (m_op_log_buffer.ReadableBytes() >= 1024 * 1024)
		{
			FlushOpLog();
		}
	}

	void OpLogs::Run()
	{
		Load();
		while (true)
		{
			Runnable* task = NULL;
			{
				LockGuard<ThreadMutexLock> guard(m_lock);
				while (m_tasks.empty())
				{
					m_lock.Wait(1000);
				}
				if (!m_tasks.empty())
				{
					task = m_tasks.front();
					m_tasks.pop_front();
				}
			}
			if (NULL != task)
			{
				task->Run();
			}
			Routine();
		}
	}

	void OpLogs::PostTask(Runnable* r)
	{
		LockGuard<ThreadMutexLock> guard(m_lock);
		m_tasks.push_back(r);
		m_lock.Notify();
	}

	void OpLogs::CheckCurrentDB(const DBID& db)
	{
		if (m_current_db != db)
		{
			//generate 'select' cmd
			m_current_db = db;
			ArgumentArray strs;
			strs.push_back("select");
			strs.push_back(m_current_db);
			SaveCmdOp(new RedisCommandFrame(strs));
		}
	}

	void OpLogs::RemoveOldestOp()
	{
		while (m_min_seq < m_max_seq)
		{
			CachedOpTable::iterator found = m_mem_op_logs.find(m_min_seq++);
			if (found != m_mem_op_logs.end())
			{
				if (found->second->type != kOtherOpType)
				{
					CachedWriteOp* op = (CachedWriteOp*) found->second;
					m_mem_op_idx.erase(op->key);
				}
				DELETE(found->second);
				m_mem_op_logs.erase(found);
				break;
			}
		}
	}

	uint64 OpLogs::SaveCmdOp(RedisCommandFrame* cmd, bool writeOpLog)
	{
		LockGuard<ThreadMutexLock> guard(m_lock);
		uint64 seq = m_max_seq++;
		CachedCmdOp* op = new CachedCmdOp(cmd);
		m_mem_op_logs[seq] = op;
		if (m_mem_op_logs.size() >= m_cfg.rep_backlog_size)
		{
			//rm head cached op
			RemoveOldestOp();
		}
		if (writeOpLog)
		{
			WriteCachedOp(seq, op);
		}

		return seq;
	}

	uint64 OpLogs::SaveWriteOp(OpKey& opkey, uint8 type, bool writeOpLog)
	{
		LockGuard<ThreadMutexLock> guard(m_lock);
		RemoveExistOp(opkey);
		uint64 seq = m_max_seq++;
		CachedWriteOp*op = new CachedWriteOp(type, opkey);
		m_mem_op_logs[seq] = op;
		m_mem_op_idx[opkey] = seq;
		if (m_mem_op_logs.size() >= m_cfg.rep_backlog_size)
		{
			//rm head cached op
			RemoveOldestOp();
		}
		if (writeOpLog)
		{
			WriteCachedOp(seq, op);
		}
		return seq;
	}

	int OpLogs::LoadOpLog(uint64& seq, Buffer& buf)
	{
		LockGuard<ThreadMutexLock> guard(m_lock);
		if (seq < m_min_seq || seq > m_max_seq)
		{
			return -1;
		}
		while (seq < m_max_seq)
		{
			uint64 current_seq = seq;
			char seqbuf[256];
			sprintf(seqbuf, "%llu", current_seq);
			seq++;
			CachedOpTable::iterator fit = m_mem_op_logs.find(current_seq);
			if (fit != m_mem_op_logs.end())
			{
				CachedOp* op = fit->second;
				if (op->type == kOtherOpType)
				{
					CachedWriteOp* writeOp = (CachedWriteOp*) op;
					ArgumentArray strs;
					strs.push_back(
							op->type == kSetOpType ? "__set__" : "__del__");
					strs.push_back(writeOp->key.key);
					if (op->type == kSetOpType)
					{
						//retrive value from db
						std::string v;
						if (0
								!= m_db->RawGet(writeOp->key.db,
										writeOp->key.key, &v))
						{
							continue;
						}
						strs.push_back(v);
					}
					//push seq at last
					strs.push_back(seqbuf);
					RedisCommandFrame cmd(strs);
					RedisCommandEncoder::Encode(buf, cmd);
				} else
				{
					CachedCmdOp* cmdop = (CachedCmdOp*) op;
					//push seq at last
					cmdop->cmd->GetArguments().push_back(seqbuf);
					RedisCommandEncoder::Encode(buf, *(cmdop->cmd));
				}
				return 1;
			}
		}
		return 0;
	}

	void OpLogs::RemoveExistOp(OpKey& key)
	{
		OpKeyIndexTable::iterator found = m_mem_op_idx.find(key);
		if (found != m_mem_op_idx.end())
		{
			uint64 seq = found->second;
			CachedOpTable::iterator fit = m_mem_op_logs.find(seq);
			if (fit != m_mem_op_logs.end())
			{
				CachedOp* op = fit->second;
				DELETE(op);
				m_mem_op_logs.erase(fit);
				m_mem_op_idx.erase(found);
			}
		}
	}

	void OpLogs::SaveSetOp(const DBID& db, const Slice& key, const Slice& value)
	{
		struct SaveTask: public Runnable
		{
				OpLogs* m_op;
				DBID dbid;
				std::string k;
				std::string v;
				SaveTask(OpLogs* op, const DBID& id, const Slice& ks,
						const Slice& vs) :
						m_op(op), dbid(id), k(ks.data(), ks.size()), v(
								vs.data(), vs.size())
				{
				}
				void Run()
				{
					m_op->CheckCurrentDB(dbid);
					OpKey ok(dbid, k);
					m_op->SaveWriteOp(ok, kSetOpType);
					delete this;
				}
		};
		PostTask(new SaveTask(this, db, key, value));
	}
	void OpLogs::SaveDeleteOp(const DBID& db, const Slice& key)
	{
		struct DelTask: public Runnable
		{
				OpLogs* m_op;
				DBID dbid;
				std::string k;
				DelTask(OpLogs* op, const DBID& id, const Slice& ks) :
						m_op(op), dbid(id), k(ks.data(), ks.size())
				{
				}
				void Run()
				{
					m_op->CheckCurrentDB(dbid);
					OpKey ok(dbid, k);
					m_op->SaveWriteOp(ok, kDelOpType);
					delete this;
				}
		};
		PostTask(new DelTask(this, db, key));
	}
	void OpLogs::SaveFlushOp(const DBID& db)
	{
		struct FlushTask: public Runnable
		{
				OpLogs* m_op;
				DBID dbid;
				FlushTask(OpLogs* op, const DBID& id) :
						m_op(op), dbid(id)
				{
				}
				void Run()
				{
					m_op->CheckCurrentDB(dbid);
					ArgumentArray strs;
					strs.push_back("flushdb");
					m_op->SaveCmdOp(new RedisCommandFrame(strs));
					delete this;
				}
		};
		PostTask(new FlushTask(this, db));
	}

	bool OpLogs::VerifyClient(const std::string& serverKey, uint64 seq)
	{
		return false;
	}
}

