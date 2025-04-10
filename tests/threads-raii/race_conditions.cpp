#include <iostream>
#include <vector>
#include <thread>
#include <atomic>

class ComplexObject
{
public:
    int value;
    std::string name;
    std::atomic<bool> flag;

    ComplexObject(int v, const std::string &n) : value(v), name(n), flag(false) {}

    void updateValue(int newValue)
    {
        value = newValue;
    }

    void updateName(const std::string &newName)
    {
        name = newName;
    }

    void updateFlag(bool newFlag)
    {
        flag.store(newFlag, std::memory_order_relaxed);
    }
};

void updateComplexObject(ComplexObject &obj, int newValue, const std::string &newName)
{

    // HERE !!!!!!
    obj.updateValue(newValue);
    obj.updateName(newName);
    obj.updateFlag(true);

    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    obj.updateValue(newValue + 1);
}

int main()
{
    ComplexObject obj(10, "OldName");

    std::thread t1(updateComplexObject, std::ref(obj), 20, "NewName1");
    std::thread t2(updateComplexObject, std::ref(obj), 30, "NewName2");

    t1.join();
    t2.join();

    std::cout << "Final value: " << obj.value << "\n";
    std::cout << "Final name: " << obj.name << "\n";
    std::cout << "Final flag: " << obj.flag.load() << "\n";

    return 0;
}
