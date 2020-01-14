#include <string>
#include <stdexcept>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <stdlib.h>
#include <gmp.h>
#include <assert.h>
#include <thread>
#include "calcwit.h"
#include "utils.h"

Circom_CalcWit::Circom_CalcWit(Circom_Circuit *aCircuit) {
    circuit = aCircuit;

#ifdef SANITY_CHECK
    signalAssigned = new bool[circuit->NSignals];
    signalAssigned[0] = true;
#endif

    mutexes = new std::mutex[NMUTEXES];
    cvs = new std::condition_variable[NMUTEXES];
    inputSignalsToTrigger = new int[circuit->NComponents];
    signalValues = new BigInt[circuit->NSignals];

    // Set one signal
    mpz_init_set_ui(signalValues[0], 1);

    // Initialize remaining signals
    for (int i=1; i<circuit->NSignals; i++) mpz_init2(signalValues[i], 256);

    BigInt p;
    mpz_init_set_str(p, circuit->P, 10);
    field = new ZqField(&p);
    mpz_clear(p);

    reset();
}


Circom_CalcWit::~Circom_CalcWit() {
    delete field;

#ifdef SANITY_CHECK
    delete signalAssigned;
#endif

    delete[] cvs;
    delete[] mutexes;

    for (int i=0; i<circuit->NSignals; i++) mpz_clear(signalValues[i]);

    delete[] signalValues;
    delete[] inputSignalsToTrigger;

}

void Circom_CalcWit::syncPrintf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    printf_mutex.lock();
    vprintf(format, args);
    printf_mutex.unlock();

    va_end(args);
}

void Circom_CalcWit::reset() {

#ifdef SANITY_CHECK
    for (int i=1; i<circuit->NComponents; i++) signalAssigned[i] = false;
#endif

    for (int i=0; i<circuit->NComponents; i++) {
        inputSignalsToTrigger[i] = circuit->components[i].inputSignals;
        if (inputSignalsToTrigger[i] == 0) triggerComponent(i);
    }
}


int Circom_CalcWit::getSubComponentOffset(int cIdx, u64 hash) {
    int hIdx;
    for(hIdx = int(hash & 0xFF); hash!=circuit->components[cIdx].hashTable[hIdx].hash; hIdx++) {
        if (!circuit->components[cIdx].hashTable[hIdx].hash) throw std::runtime_error("hash not found: " + int_to_hex(hash));
    }
    int entryPos = circuit->components[cIdx].hashTable[hIdx].pos;
    if (circuit->components[cIdx].entries[entryPos].type != _typeComponent) {
        throw std::runtime_error("invalid type");
    }
    return circuit->components[cIdx].entries[entryPos].offset;
}


Circom_Sizes Circom_CalcWit::getSubComponentSizes(int cIdx, u64 hash) {
    int hIdx;
    for(hIdx = int(hash & 0xFF); hash!=circuit->components[cIdx].hashTable[hIdx].hash; hIdx++) {
        if (!circuit->components[cIdx].hashTable[hIdx].hash) throw std::runtime_error("hash not found: " + int_to_hex(hash));
    }
    int entryPos = circuit->components[cIdx].hashTable[hIdx].pos;
    if (circuit->components[cIdx].entries[entryPos].type != _typeComponent) {
        throw std::runtime_error("invalid type");
    }
    return circuit->components[cIdx].entries[entryPos].sizes;
}

int Circom_CalcWit::getSignalOffset(int cIdx, u64 hash) {
    int hIdx;
    for(hIdx = int(hash & 0xFF); hash!=circuit->components[cIdx].hashTable[hIdx].hash; hIdx++) {
        if (!circuit->components[cIdx].hashTable[hIdx].hash) throw std::runtime_error("hash not found: " + int_to_hex(hash));
    }
    int entryPos = circuit->components[cIdx].hashTable[hIdx].pos;
    if (circuit->components[cIdx].entries[entryPos].type != _typeSignal) {
        throw std::runtime_error("invalid type");
    }
    return circuit->components[cIdx].entries[entryPos].offset;
}

Circom_Sizes Circom_CalcWit::getSignalSizes(int cIdx, u64 hash) {
    int hIdx;
    for(hIdx = int(hash & 0xFF); hash!=circuit->components[cIdx].hashTable[hIdx].hash; hIdx++) {
        if (!circuit->components[cIdx].hashTable[hIdx].hash) throw std::runtime_error("hash not found: " + int_to_hex(hash));
    }
    int entryPos = circuit->components[cIdx].hashTable[hIdx].pos;
    if (circuit->components[cIdx].entries[entryPos].type != _typeSignal) {
        throw std::runtime_error("invalid type");
    }
    return circuit->components[cIdx].entries[entryPos].sizes;
}

PBigInt Circom_CalcWit::allocBigInts(int n) {
    PBigInt res = new BigInt[n];
    for (int i=0; i<n; i++) mpz_init2(res[i], 256);
    return res;
}

void Circom_CalcWit::freeBigInts(PBigInt bi, int n) {
    for (int i=0; i<n; i++) mpz_clear(bi[i]);
    delete[] bi;
}

void Circom_CalcWit::getSignal(int currentComponentIdx, int cIdx, int sIdx, PBigInt value) {
    // syncPrintf("getSignal: %d\n", sIdx);
    if ((circuit->components[cIdx].newThread)&&(currentComponentIdx != cIdx)) {
        std::unique_lock<std::mutex> lk(mutexes[cIdx % NMUTEXES]);
        while (inputSignalsToTrigger[cIdx] != -1) {
            cvs[cIdx % NMUTEXES].wait(lk);
        }
        // cvs[cIdx % NMUTEXES].wait(lk, [&]{return inputSignalsToTrigger[cIdx] == -1;});
        lk.unlock();
    }
#ifdef SANITY_CHECK
    if (signalAssigned[sIdx] == false) {
        fprintf(stderr, "Accessing a not assigned signal: %d\n", sIdx);
        assert(false);
    }
#endif
    mpz_set(*value, signalValues[sIdx]);
    /*
    char *valueStr = mpz_get_str(0, 10, *value);
    syncPrintf("%d, Get %d --> %s\n", currentComponentIdx, sIdx, valueStr);
    free(valueStr);
    */
}

void Circom_CalcWit::finished(int cIdx) {
    {
        std::lock_guard<std::mutex> lk(mutexes[cIdx % NMUTEXES]);
        inputSignalsToTrigger[cIdx] = -1;
    }
    // syncPrintf("Finished: %d\n", cIdx);
    cvs[cIdx % NMUTEXES].notify_all();
}

void Circom_CalcWit::setSignal(int currentComponentIdx, int cIdx, int sIdx, PBigInt value) {
    // syncPrintf("setSignal: %d\n", sIdx);

#ifdef SANITY_CHECK
    if (signalAssigned[sIdx] == true) {
        fprintf(stderr, "Signal assigned twice: %d\n", sIdx);
        assert(false);
    }
    signalAssigned[sIdx] = true;
#endif
    // Log assignement
    /*
    char *valueStr = mpz_get_str(0, 10, *value);
    syncPrintf("%d, Set %d --> %s\n", currentComponentIdx, sIdx, valueStr);
    free(valueStr);
    */
    mpz_set(signalValues[sIdx], *value);
    if ( BITMAP_ISSET(circuit->mapIsInput, sIdx) ) {
        if (inputSignalsToTrigger[cIdx]>0) {
            inputSignalsToTrigger[cIdx]--;
            if (inputSignalsToTrigger[cIdx] == 0) triggerComponent(cIdx);
        }
    }

}

void Circom_CalcWit::checkConstraint(int currentComponentIdx, PBigInt value1, PBigInt value2, char const *err) {
#ifdef SANITY_CHECK
    if (mpz_cmp(*value1, *value2) != 0) {
        char *pcV1 = mpz_get_str(0, 10, *value1);
        char *pcV2 = mpz_get_str(0, 10, *value2);
        // throw std::runtime_error(std::to_string(currentComponentIdx) + std::string(", Constraint doesn't match, ") + err + ". " + sV1 + " != " + sV2 );
        fprintf(stderr, "Constraint doesn't match, %s: %s != %s", err, pcV1, pcV2);
        free(pcV1);
        free(pcV2);
        assert(false);
    }
#endif
}


void Circom_CalcWit::triggerComponent(int newCIdx) {
    //int oldCIdx = cIdx;
    // cIdx = newCIdx;
    if (circuit->components[newCIdx].newThread) {
        // syncPrintf("Triggered: %d\n", newCIdx);
        std::thread t(circuit->components[newCIdx].fn, this, newCIdx);
        // t.join();
        t.detach();
    } else {
        (*(circuit->components[newCIdx].fn))(this, newCIdx);
    }
    // cIdx = oldCIdx;
}

void Circom_CalcWit::log(PBigInt value) {
    char *pcV = mpz_get_str(0, 10, *value);
    syncPrintf("Log: %s\n", pcV);
    free(pcV);
}

void Circom_CalcWit::join() {
    for (int i=0; i<circuit->NComponents; i++) {
        std::unique_lock<std::mutex> lk(mutexes[i % NMUTEXES]);
        while (inputSignalsToTrigger[i] != -1) {
            cvs[i % NMUTEXES].wait(lk);
        }
        // cvs[i % NMUTEXES].wait(lk, [&]{return inputSignalsToTrigger[i] == -1;});
        lk.unlock();
        // syncPrintf("Joined: %d\n", i);
    }

}


