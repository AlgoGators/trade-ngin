#include "database_interface.hpp"
#include <iostream>

int main() {
    try {
        DatabaseInterface db;
        std::cout << "Successfully connected to database!\n";
        
        // Test a simple query
        auto earliest_date = db.getEarliestDate();
        std::cout << "Earliest date in database: " << earliest_date << "\n";
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
} 