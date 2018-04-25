#include <iostream>
#include <pthread.h>
using namespace std;

class A;
struct ARG
{
    A* pThis;
    string var;
};
class A
{
    public:
        A();
        ~A();
        static void* thread(void* args);
        void  excute();
    private:
        int iCount;

};

A::A()
{
    iCount = 10;
}
A::~A()
{

}
void* A::thread(void* args)
{
    ARG *arg = (ARG*)args;
    A* pThis = arg->pThis;
    string var = arg->var;
    cout<<"传入进来的参数var: "<<var<<endl;
    cout<<"用static线程函数调用私有变量： "<<pThis->iCount<<endl;

}

void A::excute()
{
    int error;
    pthread_t thread_id;
    ARG *arg = new ARG();
    arg->pThis = this;
    arg->var = "abc";
    error = pthread_create(&thread_id, NULL, thread, (void*)arg);
    if (error == 0)
    {
        cout<<"线程创建成功"<<endl;
        pthread_join(thread_id, NULL);
    }
}
int main()
{
    A a;
    a.excute();
    return 0;
}
