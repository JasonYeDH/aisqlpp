#ifndef _CONNECTION_HPP_
#define _CONNECTION_HPP_

#include "aisql_general.hpp"
#include <cppconn/driver.h>
#include <cppconn/connection.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>

#include <boost/noncopyable.hpp>
#include <boost/enable_shared_from_this.hpp>

#include <vector>

namespace aisqlpp {

class conns_manage;

class connection;
using connection_ptr = boost::shared_ptr<connection>;

class connection final: public boost::noncopyable
{
public:
    connection() = delete;
    connection(conns_manage& manage, size_t conn_uuid,
               string host, string user, string passwd, string db);
    ~connection();
    void set_uuid(size_t uuid) { conn_uuid_ = uuid; }
    size_t get_uuid() { return conn_uuid_; }

    /**
     * SQL API
     */
    bool execute_command(const string& sql);
    bool execute_query(const string& sql);
    size_t execute_query_count(const string& sql);
    bool execute_check_exist(const string& sql);
    // 不会修改内部指针引用计数
    sql::ResultSet* get_result_set() { return result_.get(); }

    // prepared stmt 
    void create_prep_stmt(const string& sql) { prep_stmt_.reset(conn_->prepareStatement(sql)); }
    sql::PreparedStatement* get_prep_stmt() { return prep_stmt_.get(); }
    bool execute_prep_stmt_command();
    bool execute_prep_stmt_query();

    template <typename T>
    bool execute_query_column(const string& sql, std::vector<T>& vec);
    template <typename T>
    bool execute_query_value(const string& sql, T& val);

    // 可变模板参数进行查询
    template<typename ... Args>
    bool execute_query_values(const string& sql, Args& ... rest);

private:
    template <typename T>
    bool raw_query_value(const uint32_t idx, T& val);
    template <typename T, typename ... Args>
    bool raw_query_value(const uint32_t idx, T& val, Args& ... rest);

private:
    sql::Driver* driver_;

    size_t  conn_uuid_; // RAND_MAX
    boost::shared_ptr<sql::Connection>  conn_;
    boost::shared_ptr<sql::Statement>   stmt_;

    // 因为都是获取连接之后查询，然后释放连接，所以这里
    // 是线程安全的，在下一次查询需要使用result_set的时候
    // 先进行reset()就可以清空之前的查询数据集，接收新的结果
    // 所以这里不会导致内存泄漏
    boost::shared_ptr<sql::ResultSet>   result_;

    // prep_stmt_ create manually
    boost::shared_ptr< sql::PreparedStatement > prep_stmt_;

    // may be used future
    conns_manage&    manage_;
};

template <typename T>
bool connection::raw_query_value(const uint32_t idx, T& val)
{
    if (typeid(T) == typeid(float) ||
        typeid(T) == typeid(double) )
    {
        val = static_cast<T>(result_->getDouble(idx));
    }
    else if (typeid(T) == typeid(int) ||
        typeid(T) == typeid(int64_t) )
    {
        val = static_cast<T>(result_->getInt64(idx));
    }
    else if (typeid(T) == typeid(unsigned int) ||
        typeid(T) == typeid(uint64_t) )
    {
        val = static_cast<T>(result_->getUInt64(idx));
    }
    else
    {
        BOOST_LOG_T(error) << "Unsupported type: " << typeid(T).name() << endl;
        return false;
    }

    return true;
}

// 字符串特例化
// 特例化如果多次包含连接会重复定义，所以要么static、inline，要不
// 这里extern进行模板声明，然后在cpp文件中进行定义
template <>
inline bool connection::raw_query_value(const uint32_t idx, std::string& val)
{
    val = static_cast<std::string>(result_->getString(static_cast<int32_t>(idx)));

    return true;
}

// APIs
template <typename T>
bool connection::execute_query_column(const string& sql, std::vector<T>& vec)
{
    try {

        if(!conn_->isValid()) 
            conn_->reconnect();

        stmt_->execute(sql);
        result_.reset(stmt_->getResultSet());
        if (result_->rowsCount() == 0)
            return false;

        vec.clear();
        T r_val;
        bool ret_flag = false;
        while (result_->next()) 
        {
            if (raw_query_value(1, r_val)) 
            {
                vec.push_back(r_val);
                ret_flag = true;
            }
        }
        return ret_flag;

    } catch (sql::SQLException &e) 
    {
        BOOST_LOG_T(error) << " STMT: " << sql << endl;
        BOOST_LOG_T(error) << "# ERR: " << e.what() << endl;
        BOOST_LOG_T(error) << " (MySQL error code: " << e.getErrorCode() << endl;
        BOOST_LOG_T(error) << ", SQLState: " << e.getSQLState() << " )" << endl;

        return false;
    }
}

template <typename T>
bool connection::execute_query_value(const string& sql, T& val)
{
    try {

        if(!conn_->isValid()) 
            conn_->reconnect();

        stmt_->execute(sql);
        result_.reset(stmt_->getResultSet());
        if (result_->rowsCount() == 0)
            return false;

        if (result_->rowsCount() != 1) 
        {
            BOOST_LOG_T(error) << "Error rows count:" << result_->rowsCount() << endl;
            return false;
        }

        if (result_->next())
            return raw_query_value(1, val);

        return false;

    } catch (sql::SQLException &e) 
    {
        BOOST_LOG_T(error) << " STMT: " << sql << endl;
        BOOST_LOG_T(error) << "# ERR: " << e.what() << endl;
        BOOST_LOG_T(error) << " (MySQL error code: " << e.getErrorCode() << endl;
        BOOST_LOG_T(error) << ", SQLState: " << e.getSQLState() << " )" << endl;

        return false;
    }
}



// 可变模板参数进行查询

template <typename T, typename ... Args>
bool connection::raw_query_value(const uint32_t idx, T& val, Args& ... rest)
{
    raw_query_value(idx, val);

    return raw_query_value(idx+1, rest ...);
}

template <typename ... Args>
bool connection::execute_query_values(const string& sql, Args& ... rest)
{
    try {

        if(!conn_->isValid()) 
            conn_->reconnect();

        stmt_->execute(sql);
        result_.reset(stmt_->getResultSet());
        if (result_->rowsCount() == 0)
            return false;

        if (result_->rowsCount() != 1) 
        {
            BOOST_LOG_T(error) << "Error rows count:" << result_->rowsCount() << endl;
            return false;
        }

        if (result_->next())
            return raw_query_value(1, rest ...);

        return false;

    } catch (sql::SQLException &e) 
    {
        BOOST_LOG_T(error) << " STMT: " << sql << endl;
        BOOST_LOG_T(error) << "# ERR: " << e.what() << endl;
        BOOST_LOG_T(error) << " (MySQL error code: " << e.getErrorCode() << endl;
        BOOST_LOG_T(error) << ", SQLState: " << e.getSQLState() << " )" << endl;

        return false;
    }
}

}

#endif  // _CONNECTION_HPP_
