#include <Python.h>
#include <iostream>
#include <vector>

using namespace std;

void runPythonScript(const vector<vector<double>>& ohlcv) {
    Py_Initialize();

    PyObject* pModule = PyImport_ImportModule("trading_script");
    if (!pModule) {
        cerr << "Error: Could not import module 'trading_script'\n";
        PyErr_Print();
        Py_Finalize();
        return;
    }

    PyObject* pFunc = PyObject_GetAttrString(pModule, "process_ohlcv");
    if (!pFunc || !PyCallable_Check(pFunc)) {
        cerr << "Error: Could not find function 'process_ohlcv'\n";
        PyErr_Print();
        Py_DECREF(pModule);
        Py_Finalize();
        return;
    }

    PyObject* pList = PyList_New(ohlcv.size());
    for (size_t i = 0; i < ohlcv.size(); ++i) {
        PyObject* row = PyList_New(ohlcv[i].size());
        for (size_t j = 0; j < ohlcv[i].size(); ++j) {
            PyList_SetItem(row, j, PyFloat_FromDouble(ohlcv[i][j]));
        }
        PyList_SetItem(pList, i, row);
    }

    PyObject* pArgs = PyTuple_Pack(1, pList);
    PyObject* pResult = PyObject_CallObject(pFunc, pArgs);

    if (pResult) {
        cout << "Python function executed successfully.\n";
        Py_DECREF(pResult);
    } else {
        cerr << "Error: Python function call failed.\n";
        PyErr_Print();
    }

    Py_DECREF(pArgs);
    Py_DECREF(pList);
    Py_DECREF(pFunc);
    Py_DECREF(pModule);
    
    Py_Finalize();
}

int main() {
    //Need to replace with real
    vector<vector<double>> ohlcv = {
        {100.5, 102.3, 99.7, 101.2, 1500},
        {101.2, 103.5, 100.9, 102.8, 2000},
        {102.8, 104.0, 101.5, 103.2, 1800},
    };

    runPythonScript(ohlcv);

    return 0;
}
