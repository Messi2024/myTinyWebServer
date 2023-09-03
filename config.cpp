#include "config.h"

Config::Config()
{
    //端口号,默认9006
    PORT = 9006;

    //日志写入方式，默认同步
    LOGWrite = 0;

    //触发组合模式， 默认都是LT
    TRIGMode = 0;

    //listenfd触发模式，默认LT
    LISTENTrigmode = 0;

    //connfd触发模式，默认LT
    CONNTrigmode = 0;

    //默认不使用优雅关闭
    OPT_LINGER = 0;

    //数据库连接池中连接数量默认为8
    sql_num = 8;

    //线程池中线程数默认为8
    thread_num = 8;

    //默认不关闭日志
    close_log = 0;

    //并发模型默认为模拟proactor
    actor_model = 0;
}

void Config::parse_arg(int argc, char *argv[])
{
    int opt;
    //单字符后加：表示之后必须带一个参数
    const char* str  = "p:l:m:o:s:t:c:a:";
    //获取参数列表
    while((opt = getopt(argc, argv, str)) != -1)
    {
        switch(opt)
        {
        case 'p':
        {
            PORT = atoi(optarg);
            break;
        }
        case 'l':
        {
            LOGWrite = atoi(optarg);
            break;
        }
        case 'm':
        {
            OPT_LINGER = atoi(optarg);
            break;
        }
        case 's':
        {
            sql_num = atoi(optarg);
            break;
        }
        case 't':
        {
            thread_num = atoi(optarg);
            break;
        }
        case 'a':
        {
            actor_model = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
}