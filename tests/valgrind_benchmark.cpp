#include <gtest/gtest.h>
#include <vector>
#include <memory>

class Resource {
    std::vector<int> data;
public:
    Resource(size_t size) : data(size) {}
    void fill(int value) { std::fill(data.begin(), data.end(), value); }
    size_t size() const { return data.size(); }
};

class ResourceManager {
    std::vector<Resource*> resources;
public:
    void add_resource(size_t size) {
        resources.push_back(new Resource(size));  // Deliberate memory leak
    }
    
    ~ResourceManager() {
        // Incomplete cleanup - some resources might be leaked
        for (size_t i = 0; i < resources.size() / 2; ++i) {
            delete resources[i];
        }
    }
};

TEST(MemoryTest, ResourceLeak) {
    ResourceManager manager;
    for (int i = 0; i < 10; ++i) {
        manager.add_resource(1000);
    }
}

TEST(MemoryTest, ProperRAII) {
    std::vector<std::unique_ptr<Resource>> resources;
    for (int i = 0; i < 10; ++i) {
        resources.push_back(std::make_unique<Resource>(1000));
    }
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 