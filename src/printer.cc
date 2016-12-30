//
// printer.cc
//

// This file contains the interface for the Printer class and some
// functions that will be used by both the SM and QL components.

#include "printer.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <glog/logging.h>

using namespace std;

//
// void Spaces(int maxLength, int printedSoFar)
//
// This method will output some spaces so that print entry will align everythin
// nice and neat.
//
void Spaces(int maxLength, size_t printedSoFar) {
    for (int i = printedSoFar; i < maxLength; i++)
        cout << " ";
}

//
// ------------------------------------------------------------------------------
//

//
// Printer
//
// This class handles the printing of tuples.
//
//  DataAttrInfo - describes all of the attributes. Defined
//      within sm.h
//  attrCount - the number of attributes
//
Printer::Printer(const std::vector<DataAttrInfo> &attributes_) {
    attrCount = attributes_.size();
    attributes = new DataAttrInfo[attrCount];

    for (int i = 0; i < attrCount; i++)
        attributes[i] = attributes_[i];

    // Number of tuples printed
    iCount = 0;

    // Figure out what the header information will look like.  Normally,
    // we can just use the attribute name, but if that appears more than
    // once, then we should use "relation.attribute".

    // this line broke when using CC
    // changing to use malloc and free instead of new and delete
    // psHeader = (char **) new (char *)[attrCount];
    psHeader = (char**)malloc(attrCount * sizeof(char*));

    // Also figure out the number of spaces between each attribute
    spaces = new int[attrCount];

    for (int i = 0; i < attrCount; i++ ) {
        // Try to find the attribute in another column
        int bFound = 0;
        psHeader[i] = new char[MAXPRINTSTRING];
        memset(psHeader[i], 0, MAXPRINTSTRING);

        for (int j = 0; j < attrCount; j++)
            if (j != i &&
                    strcmp(attributes[i].attrName,
                           attributes[j].attrName) == 0) {
                bFound = 1;
                break;
            }

        if (bFound)
            sprintf(psHeader[i], "%s.%s",
                    attributes[i].relName, attributes[i].attrName);
        else
            strcpy(psHeader[i], attributes[i].attrName);

        if (attributes[i].attrType == STRING)
            spaces[i] = min(attributes[i].attrDisplayLength, MAXPRINTSTRING);
        else
            spaces[i] = max(12, (int)strlen(psHeader[i]));

        // We must subtract out those characters that will be for the
        // header.
        spaces[i] -= strlen(psHeader[i]);

        // If there are negative (or zero) spaces, then insert a single
        // space.
        if (spaces[i] < 1) {
            // The psHeader will give us the space we need
            spaces[i] = 0;
            strcat(psHeader[i], " ");
        }
    }
}


//
// Destructor
//
Printer::~Printer() {
    for (int i = 0; i < attrCount; i++)
        delete [] psHeader[i];

    delete [] spaces;
    //delete [] psHeader;
    free (psHeader);
    delete [] attributes;
}

//
// PrintHeader
//
void Printer::PrintHeader( ostream &c ) const {
    int dashes = 0;
    int iLen;
    int i, j;

    for (i = 0; i < attrCount; i++) {
        // Print out the header information name
        c << psHeader[i];
        iLen = (int)strlen(psHeader[i]);
        dashes += iLen;

        for (j = 0; j < spaces[i]; j++)
            c << " ";

        dashes += spaces[i];
    }

    c << "\n";
    for (i = 0; i < dashes; i++) c << "-";
    c << "\n";
}

//
// PrintFooter
//
void Printer::PrintFooter(ostream &c) const {
    c << "\n";
    c << iCount << " tuple(s).\n";
}

//
// Print
//
//  data - the actual data for the tuple to be printed
//
//  The routine tries to make things line up nice, however no
//  attempt is made to keep the tuple constrained to some number of
//  characters.
//
void Printer::Print(ostream &c, const char * const data, bool isnull[]) {
    char str[MAXPRINTSTRING], strSpace[50];
    int i, a;
    float b;

    if (data == NULL)
        return;

    // Increment the number of tuples printed
    iCount++;

    int nullableIndex = 0;

    for (i = 0; i < attrCount; i++) {
        bool this_isnull = false;
        if (!(attributes[i].attrSpecs & ATTR_SPEC_NOTNULL)) {
            this_isnull = isnull[nullableIndex++];
        }
        if (attributes[i].attrType == STRING || this_isnull) {
            // We will only print out the first MAXNAME+10 characters of
            // the string value.
            memset(str, 0, MAXPRINTSTRING);

            const char* str_to_print = this_isnull ? "NULL" : data + attributes[i].offset;

            if (attributes[i].attrDisplayLength > MAXPRINTSTRING) {
                strncpy(str, str_to_print, MAXPRINTSTRING - 1);
                str[MAXPRINTSTRING - 3] = '.';
                str[MAXPRINTSTRING - 2] = '.';
                c << str;
                Spaces(MAXPRINTSTRING, strlen(str));
            } else {
                strncpy(str, str_to_print, (size_t)(this_isnull ? 4 : attributes[i].attrDisplayLength));
                c << str;
                if (attributes[i].attrDisplayLength < (int) strlen(psHeader[i]))
                    Spaces((int)strlen(psHeader[i]), strlen(str));
                else
                    Spaces(attributes[i].attrDisplayLength, strlen(str));
            }
        } else if (attributes[i].attrType == INT) {
            memcpy (&a, (data + attributes[i].offset), sizeof(int));
            sprintf(strSpace, "%d", a);
            c << a;
            if (strlen(psHeader[i]) < 12)
                Spaces(12, strlen(strSpace));
            else
                Spaces((int)strlen(psHeader[i]), strlen(strSpace));
        } else if (attributes[i].attrType == FLOAT) {
            memcpy (&b, (data + attributes[i].offset), sizeof(float));
            sprintf(strSpace, "%f", b);
            c << strSpace;
            if (strlen(psHeader[i]) < 12)
                Spaces(12, strlen(strSpace));
            else
                Spaces((int)strlen(psHeader[i]), strlen(strSpace));
        }
    }
    c << "\n";
}

