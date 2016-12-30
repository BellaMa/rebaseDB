//
// Created by Kanari on 2016/12/24.
//

#include <set>
#include <cassert>
#include <numeric>
#include "ql.h"
#include "ql_graph.h"

QL_Manager::QL_Manager(SM_Manager &smm, IX_Manager &ixm, RM_Manager &rmm) {
    pSmm = &smm;
    pIxm = &ixm;
    pRmm = &rmm;
}

QL_Manager::~QL_Manager() {
}

static bool can_assign_to(AttrType rt, ValueType vt, bool nullable) {
    return (vt == VT_NULL && nullable) ||
           (vt == VT_INT && rt == INT) ||
           (vt == VT_INT && rt == FLOAT) ||
           (vt == VT_FLOAT && rt == FLOAT) ||
           (vt == VT_STRING && rt == STRING);
}

inline AttrTag make_tag(const RelAttr &info) {
    return AttrTag(info.relName ? std::string(info.relName) : "",
                   std::string(info.attrName));
};

#define DEFINE_ATTRINFO(_name, _key) \
    auto __iter__##_name = attrMap.find(_key); \
    if (__iter__##_name == attrMap.end()) return QL_ATTR_NOTEXIST; \
    DataAttrInfo &_name = __iter__##_name->second;

RC QL_Manager::Select(int nSelAttrs, const RelAttr *selAttrs,
                      int nRelations, const char *const *relations,
                      int nConditions, const Condition *conditions) {
    // open files
    ARR_PTR(fileHandles, RM_FileHandle, nRelations);
    for (int i = 0; i < nRelations; ++i)
        TRY(pRmm->OpenFile(relations[i], fileHandles[i]));
    ARR_PTR(relEntries, RelCatEntry, nRelations);
    for (int i = 0; i < nRelations; ++i)
        TRY(pSmm->GetRelEntry(relations[i], relEntries[i]));
    VLOG(1) << "files opened";

    /**
     * Check if query is valid
     */
    // create mappings of attribute names to corresponding info
    ARR_PTR(attrInfo, std::vector<DataAttrInfo>, nRelations);
    ARR_PTR(attrCount, int, nRelations);
    std::map<std::string, int> attrNameCount;
    AttrMap<DataAttrInfo> attrMap;
    for (int i = 0; i < nRelations; ++i) {
        TRY(pSmm->GetDataAttrInfo(relations[i], attrCount[i], attrInfo[i], true));
        std::string relName(relations[i]);
        for (int j = 0; j < attrCount[i]; ++j) {
            std::string attrName(attrInfo[i][j].attrName);
            attrMap[std::make_pair(relName, attrName)] = attrInfo[i][j];
            ++attrNameCount[attrName];
        }
    }
    for (int i = 0; i < nRelations; ++i)
        for (int j = 0; j < attrCount[i]; ++j) {
            std::string attrName(attrInfo[i][j].attrName);
            if (attrNameCount[attrName] == 1)
                attrMap[std::make_pair(std::string(), attrName)] = attrInfo[i][j];
        }
    VLOG(1) << "attribute name mapping created";

    // check selected attributes exist
    if (nSelAttrs == 1 && !strcmp(selAttrs[0].attrName, "*"))
        nSelAttrs = 0;
    for (int i = 0; i < nSelAttrs; ++i) {
        DEFINE_ATTRINFO(_, make_tag(selAttrs[i]));
        if (selAttrs[i].relName == NULL && attrNameCount[std::string(selAttrs[i].attrName)] > 1)
            return QL_AMBIGUOUS_ATTR_NAME;
    }
    VLOG(1) << "all attributes exist";

    // check conditions are valid
    for (int i = 0; i < nConditions; ++i) {
        DEFINE_ATTRINFO(lhsAttr, make_tag(conditions[i].lhsAttr));
        bool nullable = ((lhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) == 0);
        VLOG(1) << lhsAttr.attrType << " " << conditions[i].rhsValue.type << " " << nullable;
        if (conditions[i].bRhsIsAttr) {
            DEFINE_ATTRINFO(rhsAttr, make_tag(conditions[i].rhsAttr));
            if (lhsAttr.attrType != rhsAttr.attrType) {
                return QL_ATTR_TYPES_MISMATCH;
            }
        } else {
            if (!can_assign_to(lhsAttr.attrType, conditions[i].rhsValue.type, nullable))
                return QL_VALUE_TYPES_MISMATCH;
        }
    }
    VLOG(1) << "all conditions are valid";

    /**
     * Naïve nested-loop join
     */
    std::map<std::string, int> relNumMap;
    for (int i = 0; i < nRelations; ++i)
        relNumMap[std::string(relations[i])] = i;
    std::vector<QL_Condition> conds;
    ARR_PTR(lhsAttrRelIndex, int, nConditions);
    ARR_PTR(rhsAttrRelIndex, int, nConditions);
    for (int i = 0; i < nConditions; ++i) {
        DataAttrInfo &lhsAttr = attrMap[make_tag(conditions[i].lhsAttr)];
        lhsAttrRelIndex[i] = relNumMap[lhsAttr.relName];
        QL_Condition cond;
        cond.lhsAttr = lhsAttr;
        cond.op = conditions[i].op;
        cond.bRhsIsAttr = (bool)conditions[i].bRhsIsAttr;
        if (conditions[i].bRhsIsAttr) {
            DataAttrInfo &rhsAttr = attrMap[make_tag(conditions[i].rhsAttr)];
            rhsAttrRelIndex[i] = relNumMap[rhsAttr.relName];
            cond.rhsAttr = rhsAttr;
        } else {
            cond.rhsValue = conditions[i].rhsValue;
        }
        conds.push_back(cond);
    }
    ARR_PTR(fileScans, RM_FileScan, nRelations);
    ARR_PTR(records, RM_Record, nRelations);
    ARR_PTR(data, char *, nRelations);
    ARR_PTR(isnull, bool *, nRelations);
    VLOG(1) << "conditions processed";

    std::vector<DataAttrInfo> projections((unsigned long)nSelAttrs);
    std::vector<DataAttrInfo> finalHeaders((unsigned long)nSelAttrs);
    ARR_PTR(selAttrRelIndex, int, nSelAttrs);
    int finalRecordSize = 0, nullableIndex = 0;
    if (nSelAttrs == 0) {
        nSelAttrs = (int)std::accumulate(attrCount, attrCount + nRelations, 0UL);
        projections.resize(nSelAttrs);
        finalHeaders.resize(nSelAttrs);
        int attrCnt = 0;
        for (int i = 0; i < nRelations; ++i)
            for (int j = 0; j < attrCount[i]; ++j)
                projections[attrCnt++] = attrInfo[i][j];
    } else {
        for (int i = 0; i < nSelAttrs; ++i)
            projections[i] = attrMap[make_tag(selAttrs[i])];
    }
    for (int i = 0; i < nSelAttrs; ++i) {
        finalHeaders[i] = projections[i];
        finalHeaders[i].offset = finalRecordSize;
        selAttrRelIndex[i] = relNumMap[projections[i].relName];
        finalRecordSize += upper_align<4>(projections[i].attrSize);
        if ((projections[i].attrSpecs & ATTR_SPEC_NOTNULL) == 0) {
            finalHeaders[i].nullableIndex = nullableIndex++;
        } else {
            finalHeaders[i].nullableIndex = -1;
        }
    }
    ARR_PTR(finalRecordData, char, finalRecordSize);
    ARR_PTR(finalRecordIsnull, bool, nullableIndex);
    VLOG(1) << "projections processed";

    int sumRecords = 1;
    for (int i = 0; i < nRelations; ++i)
        sumRecords *= relEntries[i].recordCount;
    VLOG(1) << "sumRecords=" << sumRecords;

//    VLOG(1) << finalRecordSize;
//    for (int i = 0; i < nSelAttrs; ++i) {
//        VLOG(1) << selAttrRelIndex[i] << " " << projections[i].offset << " " << projections[i].attrSize;
//        VLOG(1) << finalHeaders[i].offset << " " << finalHeaders[i].nullableIndex;
//    }

    Printer printer(finalHeaders);
    printer.PrintHeader(std::cout);
    int ptr = 0;
    int cnt = 0;
    TRY(fileScans[ptr].OpenScan(fileHandles[0], INT, 4, 0, NO_OP, NULL));
    while (true) {
        int retcode = RM_EOF;
        while (retcode == RM_EOF && ptr >= 0) {
            retcode = fileScans[ptr].GetNextRec(records[ptr]);
            if (retcode == RM_EOF) {
                TRY(fileScans[ptr].CloseScan());
                --ptr;
            } else if (retcode != 0) return retcode;
        }
        if (ptr < 0) break;
        TRY(records[ptr].GetData(data[ptr]));
        TRY(records[ptr].GetIsnull(isnull[ptr]));
        while (ptr + 1 < nRelations) {
            ++ptr;
            TRY(fileScans[ptr].OpenScan(fileHandles[ptr], INT, 4, 0, NO_OP, NULL));
            TRY(fileScans[ptr].GetNextRec(records[ptr]));
            TRY(records[ptr].GetData(data[ptr]));
            TRY(records[ptr].GetIsnull(isnull[ptr]));
        }
        ++cnt;
        if (cnt % ((sumRecords + 99) / 100) == 0) {
            std::cout << "[" << 100 * cnt / sumRecords << "%] " << cnt << "/" << sumRecords << "\r";
            fflush(stdout);
        }
        bool satisfy = true;
        for (int i = 0; i < nConditions && satisfy; ++i) {
            if (conds[i].bRhsIsAttr) {
                satisfy = checkSatisfy(data[lhsAttrRelIndex[i]] + conds[i].lhsAttr.offset,
                                       !(conds[i].lhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) ? isnull[lhsAttrRelIndex[i]][conds[i].lhsAttr.nullableIndex] : false,
                                       data[rhsAttrRelIndex[i]] + conds[i].rhsAttr.offset,
                                       !(conds[i].rhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) ? isnull[rhsAttrRelIndex[i]][conds[i].rhsAttr.nullableIndex] : false,
                                       conds[i]);
            } else {
                satisfy = checkSatisfy(data[lhsAttrRelIndex[i]] + conds[i].lhsAttr.offset,
                                       !(conds[i].lhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) ? isnull[lhsAttrRelIndex[i]][conds[i].lhsAttr.nullableIndex] : false,
                                       (char *)conds[i].rhsValue.data,
                                       conds[i].rhsValue.type == VT_NULL,
                                       conds[i]);
            }
        }

        if (satisfy) {
            for (int i = 0; i < nSelAttrs; ++i) {
                memcpy(finalRecordData + finalHeaders[i].offset,
                       data[selAttrRelIndex[i]] + projections[i].offset,
                       (size_t)projections[i].attrSize);
                if ((projections[i].attrSpecs & ATTR_SPEC_NOTNULL) == 0)
                    finalRecordIsnull[finalHeaders[i].nullableIndex] = isnull[selAttrRelIndex[i]][projections[i].nullableIndex];
            }
            printer.Print(std::cout, finalRecordData, finalRecordIsnull);
            std::cout << "[" << 100 * cnt / sumRecords << "%] " << cnt << "/" << sumRecords << "\r";
            fflush(stdout);
        }
    }
    std::cout << "[100%] " << sumRecords << "/" << sumRecords << "\r";
    printer.PrintFooter(std::cout);

    VLOG(1) << cnt << " records processed";
    VLOG(1) << sumRecords << " records in total";
    assert(cnt == sumRecords);

    return 0;

    /**
     * Generate query plan
     *
    std::vector<QL_QueryPlan> queryPlans;
    std::vector<std::string> temporaryTables;

    std::map<std::string, int> relNumMap;
    for (int i = 0; i < nRelations; ++i)
        relNumMap[std::string(relations[i])] = i;

    // build target projections
    ARR_PTR(targetProjections, std::vector<std::string>, nRelations);
    if (nSelAttrs == 0) {
        for (int i = 0; i < nRelations; ++i)
            for (int j = 0; j < attrCount[i]; ++j)
                targetProjections[i].push_back(std::string(attrInfo[i][j].attrName));
    } else {
        for (int i = 0; i < nSelAttrs; ++i) {
            DataAttrInfo &info = attrMap[make_tag(selAttrs[i])];
            targetProjections[relNumMap[std::string(info.relName)]].push_back(std::string(info.attrName));
        }
    }
    VLOG(1) << "target projections built";

    // gather simple conditions and projections for each table
    ARR_PTR(simpleConditions, std::vector<QL_Condition>, nRelations);
    std::vector<QL_Condition> complexConditions;
    ARR_PTR(simpleProjections, std::set<std::string>, nRelations);
    for (int i = 0; i < nRelations; ++i) {
        std::string relName(relations[i]);
        for (auto attrName : targetProjections[i])
            simpleProjections[i].insert(attrName);
    }
    for (int i = 0; i < nConditions; ++i) {
        DataAttrInfo &lhsAttr = attrMap[make_tag(conditions[i].lhsAttr)];
        int lhsAttrNum = relNumMap[std::string(lhsAttr.relName)];
        QL_Condition cond;
        cond.lhsAttr = lhsAttr;
        cond.op = conditions[i].op;
        cond.bRhsIsAttr = (bool)conditions[i].bRhsIsAttr;
        if (conditions[i].bRhsIsAttr) {
            DataAttrInfo &rhsAttr = attrMap[make_tag(conditions[i].rhsAttr)];
            cond.rhsAttr = rhsAttr;
            if (!strcmp(lhsAttr.relName, rhsAttr.relName)) {
                simpleConditions[lhsAttrNum].push_back(cond);
            } else {
                complexConditions.push_back(cond);
            }
            int rhsAttrNum = relNumMap[std::string(rhsAttr.relName)];
            simpleProjections[lhsAttrNum].insert(std::string(lhsAttr.attrName));
            simpleProjections[rhsAttrNum].insert(std::string(rhsAttr.attrName));
        } else {
            cond.rhsValue = conditions[i].rhsValue;
            simpleConditions[lhsAttrNum].push_back(cond);
            simpleProjections[lhsAttrNum].insert(std::string(lhsAttr.attrName));
        }
    }
    VLOG(1) << "simple conditions and projections gathered";

    // select and projections related to single relations
    ARR_PTR(filteredRefName, std::string, nRelations);
    for (int i = 0; i < nRelations; ++i) {
        std::string relName(relations[i]);
        int relNum = relNumMap[relName];
        auto &relCond = simpleConditions[relNum];
        if (relCond.size() > 0) {
            filteredRefName[i] = pSmm->GenerateTempTableName(relName);
            QL_QueryPlan plan;
            plan.type = QP_SCAN;
            plan.relName = relName;
            plan.tempSaveName = filteredRefName[i];
            temporaryTables.push_back(plan.tempSaveName);
            plan.conditions = relCond;
            for (auto attrName : simpleProjections[relNum])
                plan.projection.push_back(attrName);
            queryPlans.push_back(plan);
        } else {
            filteredRefName[i] = std::string(relations[i]);
        }
    }
    VLOG(1) << "query plan part 1 (simple statements) generated";

    // part relations related by conditions and join separately
    QL_Graph graph(nRelations);
    ARR_PTR(relatedConditions, std::vector<QL_Condition>, nRelations);
    for (auto condition : complexConditions) {
        int a = relNumMap[std::string(condition.lhsAttr.relName)];
        int b = relNumMap[std::string(condition.rhsAttr.relName)];
        if (a > b) std::swap(a, b);
        graph.insertEdge(a, b);
        relatedConditions[b].push_back(condition);
    }
    VLOG(1) << "relation graph built";

    std::vector<std::string> descartesProductRelations;
    for (auto block : graph) {
        QL_QueryPlan root, child;
        root.type = QP_SCAN;
        root.relName = filteredRefName[block[0]];
        root.projection = targetProjections[block[0]];
        root.tempSaveName = pSmm->GenerateTempTableName("joined_block");
        temporaryTables.push_back(root.tempSaveName);
        auto innerLoop = std::shared_ptr<QL_QueryPlan>(nullptr);
        for (int i = (int)block.size() - 1; i > 0; --i) {
            child.type = QP_SCAN;
            child.relName = filteredRefName[block[i]];
            child.projection = targetProjections[block[i]];
            child.conditions = relatedConditions[block[i]];
            child.innerLoop = innerLoop;
            innerLoop = std::make_shared<QL_QueryPlan>(child);
        }
        root.innerLoop = innerLoop;
        descartesProductRelations.push_back(root.tempSaveName);
        queryPlans.push_back(root);
    }
    VLOG(1) << "query plan part 2 (non-descartes-product-joins) generated";

    // join unrelated relations (unfiltered descartes product)
    std::string finalRelName = descartesProductRelations[0];
    if (descartesProductRelations.size() > 1) {
        QL_QueryPlan root, child;
        root.type = QP_SCAN;
        root.relName = descartesProductRelations[0];
        root.tempSaveName = pSmm->GenerateTempTableName("final");
        temporaryTables.push_back(root.tempSaveName);
        finalRelName = root.tempSaveName;
        auto innerLoop = std::shared_ptr<QL_QueryPlan>(nullptr);
        for (int i = (int)descartesProductRelations.size() - 1; i > 0; --i) {
            child.type = QP_SCAN;
            child.relName = descartesProductRelations[i];
            child.innerLoop = innerLoop;
            innerLoop = std::make_shared<QL_QueryPlan>(child);
        }
        root.innerLoop = innerLoop;
        queryPlans.push_back(root);
    }
    VLOG(1) << "query plan part 3 (descartes product join) generated";

    QL_QueryPlan final;
    final.type = QP_FINAL;
    final.relName = finalRelName;
    queryPlans.push_back(final);

    // print and execute plan
    if (bQueryPlans) {
        for (auto plan : queryPlans) {
            TRY(PrintQueryPlan(plan));
            std::cout << std::endl;
        }
    }
    VLOG(1) << "query plan printed";

    for (auto plan : queryPlans) {

    }

    // purge temporary tables
//    for (auto relName : temporaryTables)
//        TRY(pSmm->DropTable(relName.c_str()));
//    VLOG(1) << "temporary tables purged";

    return 0;
    */
}

#undef DEFINE_ATTRINFO

std::ostream &operator <<(std::ostream &os, const QL_Condition &condition) {
    if (condition.op == NO_OP) {
        os << "*";
    } else {
        os << condition.lhsAttr.relName << "." << condition.lhsAttr.attrName;
        os << " ";
        if (condition.op == ISNULL_OP || condition.op == NOTNULL_OP) {
            if (condition.op == ISNULL_OP) os << "is null";
            else os << "is not null";
        } else {
            switch (condition.op) {
                case EQ_OP:
                    os << "=";
                    break;
                case NE_OP:
                    os << "!=";
                    break;
                case LT_OP:
                    os << "<";
                    break;
                case GT_OP:
                    os << ">";
                    break;
                case LE_OP:
                    os << "<=";
                    break;
                case GE_OP:
                    os << ">=";
                    break;
                default:
                    break;
            }
            os << " ";
            if (condition.bRhsIsAttr) {
                os << condition.rhsAttr.relName << "." << condition.rhsAttr.attrName;
            } else {
                switch (condition.rhsValue.type) {
                    case VT_INT:
                        os << *(int *)condition.rhsValue.data;
                        break;
                    case VT_FLOAT:
                        os << *(float *)condition.rhsValue.data;
                        break;
                    case VT_STRING:
                        os << (char *)condition.rhsValue.data;
                        break;
                    default:
                        break;
                }
            }
        }
    }
    return os;
}

RC QL_Manager::PrintQueryPlan(const QL_QueryPlan &queryPlan, int indent) {
    std::string prefix = "";
    prefix.append((unsigned long)indent, ' ');
    switch (queryPlan.type) {
        case QP_SCAN:
            std::cout << prefix;
            std::cout << "SCAN " << queryPlan.relName;
            if (queryPlan.conditions.size() > 0) {
                std::cout << " FILTER:" << std::endl;
                for (auto cond : queryPlan.conditions)
                    std::cout << prefix << " - " << cond << std::endl;
            } else {
                std::cout << std::endl;
            }
            if (queryPlan.projection.size() > 0) {
                std::cout << prefix;
                std::cout << "> PROJECTION:";
                for (auto attrName : queryPlan.projection)
                    std::cout << " " << attrName;
                std::cout << std::endl;
            }
            if (queryPlan.innerLoop != nullptr)
                TRY(PrintQueryPlan(*queryPlan.innerLoop, indent + 4));
            if (queryPlan.tempSaveName != "") {
                std::cout << prefix;
                std::cout << "=> SAVING AS " << queryPlan.tempSaveName;
                std::cout << std::endl;
            }
            break;
        case QP_SEARCH:
            std::cout << prefix;
            std::cout << "SEARCH " << queryPlan.relName;
            std::cout << " USING INDEX ON " << queryPlan.indexAttrName;
            assert(queryPlan.conditions.size() == 1);
            std::cout << " FILTER: " << queryPlan.conditions[0] << std::endl;
            if (queryPlan.projection.size() > 0) {
                std::cout << prefix;
                std::cout << "> PROJECTION:";
                for (auto attrName : queryPlan.projection)
                    std::cout << " " << attrName;
                std::cout << std::endl;
            }
            if (queryPlan.tempSaveName != "") {
                std::cout << prefix;
                std::cout << "=> SAVING AS " << queryPlan.tempSaveName;
                std::cout << std::endl;
            }
            break;
        case QP_AUTOINDEX:
            std::cout << prefix;
            std::cout << "CREATE AUTO INDEX FOR " << queryPlan.relName << "(" << queryPlan.indexAttrName << ")";
            std::cout << std::endl;
            break;
        case QP_FINAL:
            std::cout << prefix;
            std::cout << "FINAL RESULT " << queryPlan.relName;
            std::cout << std::endl;
            break;
    }
    return 0;
}

RC QL_Manager::ExecuteQueryPlan(const QL_QueryPlan &queryPlan,
                                const std::vector<RM_FileHandle> &fileHandles,
                                const std::vector<AttrRecordInfo> &attrInfo,
                                const std::vector<void *> &outerLoopData,
                                char *recordData) {
    return 0;
}

RC QL_Manager::Insert(const char *relName, int nValues, const Value *values) {
    if (!strcmp(relName, "relcat") || !strcmp(relName, "attrcat")) return QL_FORBIDDEN;
    RelCatEntry relEntry;
    TRY(pSmm->GetRelEntry(relName, relEntry));

    int attrCount;
    bool kSort = true;
    std::vector<DataAttrInfo> attributes;
    TRY(pSmm->GetDataAttrInfo(relName, attrCount, attributes, kSort));
    if (nValues != attrCount) {
        return QL_ATTR_COUNT_MISMATCH;
    }
    int nullableNum = 0;
    for (int i = 0; i < attrCount; ++i) {
        bool nullable = ((attributes[i].attrSpecs & ATTR_SPEC_NOTNULL) == 0);
        if (nullable) {
            ++nullableNum;
        }
        if (!can_assign_to(attributes[i].attrType, values[i].type, nullable)) {
            return QL_VALUE_TYPES_MISMATCH;
        }
    }

    ARR_PTR(data, char, relEntry.tupleLength);
    ARR_PTR(isnull, bool, nullableNum);
    int nullableIndex = 0;
    for (int i = 0; i < attrCount; ++i) {
        bool nullable = ((attributes[i].attrSpecs & ATTR_SPEC_NOTNULL) == 0);
        if (nullable) {
            isnull[nullableIndex++] = (values[i].type == VT_NULL);
        }
        if (values[i].type == VT_NULL) {
            continue;
        }
        auto &attr = attributes[i];
        void *value = values[i].data;
        char *dest = data + attr.offset;
        switch (attr.attrType) {
            case INT: {
                *(int *)dest = *(int *)value;
                break;
            }
            case FLOAT: {
                *(float *)dest = *(float *)value;
                break;
            }
            case STRING: {
                char *src = (char *)value;
                if (strlen(src) > attr.attrDisplayLength) return QL_STRING_VAL_TOO_LONG;
                strcpy(dest, src);
                break;
            }
        }
    }

    RM_FileHandle fh;
    RID rid;

    TRY(pRmm->OpenFile(relName, fh));
    TRY(fh.InsertRec(data, rid, isnull));
    TRY(pRmm->CloseFile(fh));

    ++relEntry.recordCount;
    TRY(pSmm->UpdateRelEntry(relName, relEntry));

    return 0;
}

bool QL_Manager::checkSatisfy(char *lhsData, bool lhsIsnull, char *rhsData, bool rhsIsnull, const QL_Condition &condition) {
    switch (condition.op) {
        case NO_OP:
            return true;
        case ISNULL_OP:
            return lhsIsnull;
        case NOTNULL_OP:
            return !lhsIsnull;
        default:
            break;
    }
    if (lhsIsnull || rhsIsnull) return false;

    switch (condition.lhsAttr.attrType) {
        case INT: {
            int lhs = *(int *)lhsData;
            int rhs = *(int *)rhsData;
            switch (condition.op) {
                case EQ_OP:
                    return lhs == rhs;
                case NE_OP:
                    return lhs != rhs;
                case LT_OP:
                    return lhs < rhs;
                case GT_OP:
                    return lhs > rhs;
                case LE_OP:
                    return lhs <= rhs;
                case GE_OP:
                    return lhs >= rhs;
                default:
                    CHECK(false);
            }
        }
        case FLOAT: {
            float lhs = *(float *)lhsData;
            float rhs = *(float *)rhsData;
            switch (condition.op) {
                case EQ_OP:
                    return lhs == rhs;
                case NE_OP:
                    return lhs != rhs;
                case LT_OP:
                    return lhs < rhs;
                case GT_OP:
                    return lhs > rhs;
                case LE_OP:
                    return lhs <= rhs;
                case GE_OP:
                    return lhs >= rhs;
                default:
                    CHECK(false);
            }
        }
        case STRING: {
            char *lhs = lhsData;
            char *rhs = rhsData;
            switch (condition.op) {
                case EQ_OP:
                    return strcmp(lhs, rhs) == 0;
                case NE_OP:
                    return strcmp(lhs, rhs) != 0;
                case LT_OP:
                    return strcmp(lhs, rhs) < 0;
                case GT_OP:
                    return strcmp(lhs, rhs) > 0;
                case LE_OP:
                    return strcmp(lhs, rhs) <= 0;
                case GE_OP:
                    return strcmp(lhs, rhs) >= 0;
                default:
                    CHECK(false);
            }
        }
    }
    return false;
}

bool QL_Manager::checkSatisfy(char *data, bool *isnull, const QL_Condition &condition) {
    if (condition.bRhsIsAttr) {
        return checkSatisfy(data + condition.lhsAttr.offset,
                            !(condition.lhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) ? isnull[condition.lhsAttr.nullableIndex] : false,
                            data + condition.rhsAttr.offset,
                            !(condition.rhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) ? isnull[condition.rhsAttr.nullableIndex] : false,
                            condition);
    } else {
        return checkSatisfy(data + condition.lhsAttr.offset,
                            !(condition.lhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) ? isnull[condition.lhsAttr.nullableIndex] : false,
                            (char *)condition.rhsValue.data,
                            condition.rhsValue.type == VT_NULL,
                            condition);
    }
}

RC checkAttrBelongsToRel(const RelAttr &relAttr, const char *relName) {
    if (relAttr.relName == NULL || !strcmp(relName, relAttr.relName)) return 0;
    return QL_ATTR_NOTEXIST;
}

RC QL_Manager::CheckConditionsValid(const char *relName, int nConditions, const Condition *conditions,
                                    const std::map<std::string, DataAttrInfo> &attrMap,
                                    std::vector<QL_Condition> &retConditions) {
    // check conditions are valid
    for (int i = 0; i < nConditions; ++i) {
        TRY(checkAttrBelongsToRel(conditions[i].lhsAttr, relName));
        if (conditions[i].bRhsIsAttr)
            TRY(checkAttrBelongsToRel(conditions[i].rhsAttr, relName));
        auto iter = attrMap.find(conditions[i].lhsAttr.attrName);
        if (iter == attrMap.end()) return QL_ATTR_NOTEXIST;
        const DataAttrInfo &lhsAttr = iter->second;

        QL_Condition cond;
        cond.lhsAttr = lhsAttr;
        cond.op = conditions[i].op;
        cond.bRhsIsAttr = (bool)conditions[i].bRhsIsAttr;

        bool nullable = ((lhsAttr.attrSpecs & ATTR_SPEC_NOTNULL) == 0);
        if (conditions[i].bRhsIsAttr) {
            iter = attrMap.find(conditions[i].rhsAttr.attrName);
            if (iter == attrMap.end()) return QL_ATTR_NOTEXIST;
            const DataAttrInfo &rhsAttr = iter->second;
            if (lhsAttr.attrType != rhsAttr.attrType)
                return QL_ATTR_TYPES_MISMATCH;
            cond.rhsAttr = rhsAttr;
        } else {
            if (!can_assign_to(lhsAttr.attrType, conditions[i].rhsValue.type, nullable))
                return QL_VALUE_TYPES_MISMATCH;
            cond.rhsValue = conditions[i].rhsValue;
        }

        retConditions.push_back(cond);
    }
    VLOG(1) << "all conditions are valid";

    return 0;
}

RC QL_Manager::Delete(const char *relName, int nConditions, const Condition *conditions) {
    if (!strcmp(relName, "relcat") || !strcmp(relName, "attrcat")) return QL_FORBIDDEN;
    RelCatEntry relEntry;
    TRY(pSmm->GetRelEntry(relName, relEntry));

    int attrCount;
    std::vector<DataAttrInfo> attributes;
    TRY(pSmm->GetDataAttrInfo(relName, attrCount, attributes, true));
    std::map<std::string, DataAttrInfo> attrMap;
    for (auto info : attributes)
        attrMap[info.attrName] = info;

    std::vector<QL_Condition> conds;
    TRY(CheckConditionsValid(relName, nConditions, conditions, attrMap, conds));

    RM_FileHandle fileHandle;
    TRY(pRmm->OpenFile(relName, fileHandle));
    RM_FileScan scan;
    TRY(scan.OpenScan(fileHandle, INT, 4, 0, NO_OP, NULL));
    RM_Record record;
    RC retcode;
    int cnt = 0;
    while ((retcode = scan.GetNextRec(record)) != RM_EOF) {
        VLOG(1);
        if (retcode) return retcode;
        char *data;
        bool *isnull;
        TRY(record.GetData(data));
        TRY(record.GetIsnull(isnull));
        bool shouldDelete = true;
        for (int i = 0; i < nConditions && shouldDelete; ++i)
            shouldDelete = checkSatisfy(data, isnull, conds[i]);
        if (shouldDelete) {
            ++cnt;
            RID rid;
            TRY(record.GetRid(rid));
            TRY(fileHandle.DeleteRec(rid));
        }
    }
    TRY(scan.CloseScan());
    TRY(pRmm->CloseFile(fileHandle));

    relEntry.recordCount -= cnt;
    TRY(pSmm->UpdateRelEntry(relName, relEntry));
    std::cout << cnt << " tuple(s) deleted." << std::endl;

    return 0;
}

RC QL_Manager::Update(const char *relName, const RelAttr &updAttr,
                      const int bIsValue, const RelAttr &rhsRelAttr, const Value &rhsValue,
                      int nConditions, const Condition *conditions) {
    if (!strcmp(relName, "relcat") || !strcmp(relName, "attrcat")) return QL_FORBIDDEN;
    RelCatEntry relEntry;
    TRY(pSmm->GetRelEntry(relName, relEntry));

    TRY(checkAttrBelongsToRel(updAttr, relName));
    if (!bIsValue)
        TRY(checkAttrBelongsToRel(rhsRelAttr, relName));

    int attrCount;
    std::vector<DataAttrInfo> attributes;
    TRY(pSmm->GetDataAttrInfo(relName, attrCount, attributes, true));
    std::map<std::string, DataAttrInfo> attrMap;
    for (auto info : attributes)
        attrMap[info.attrName] = info;

    std::vector<QL_Condition> conds;
    TRY(CheckConditionsValid(relName, nConditions, conditions, attrMap, conds));
    for (int i = 0; i < nConditions; ++i)
        if (!(conds[i].lhsAttr.attrSpecs & ATTR_SPEC_NOTNULL))
            VLOG(1) << conds[i].lhsAttr.nullableIndex;

    DataAttrInfo updAttrInfo = attrMap[updAttr.attrName];
    int valAttrOffset = bIsValue ? 0 : attrMap[rhsRelAttr.attrName].offset;
    bool nullable = !(updAttrInfo.attrSpecs & ATTR_SPEC_NOTNULL);
    if (!nullable && bIsValue && rhsValue.type == VT_NULL)
        return QL_ATTR_IS_NOTNULL;
    if (nullable)
        VLOG(1) << updAttrInfo.attrName << " nullableIndex: " << updAttrInfo.nullableIndex;

    RM_FileHandle fileHandle;
    TRY(pRmm->OpenFile(relName, fileHandle));
    RM_FileScan scan;
    TRY(scan.OpenScan(fileHandle, INT, 4, 0, NO_OP, NULL));
    RM_Record record;
    RC retcode;
    int cnt = 0;
    while ((retcode = scan.GetNextRec(record)) != RM_EOF) {
        VLOG(1) << "next rec";
        if (retcode) return retcode;
        char *data;
        bool *isnull;
        TRY(record.GetData(data));
        TRY(record.GetIsnull(isnull));
        bool shouldUpdate = true;
        for (int i = 0; i < nConditions && shouldUpdate; ++i)
            shouldUpdate = checkSatisfy(data, isnull, conds[i]);
        if (shouldUpdate) {
            ++cnt;
            if (bIsValue && rhsValue.type == VT_NULL) {
                isnull[updAttrInfo.nullableIndex] = true;
            } else {
                VLOG(1) << "update";
                if (nullable) isnull[updAttrInfo.nullableIndex] = false;
                void *value = bIsValue ? rhsValue.data : data + valAttrOffset;
                switch (updAttrInfo.attrType) {
                    case INT:
                        *(int *)(data + updAttrInfo.offset) = *(int *)value;
                        break;
                    case FLOAT:
                        *(float *)(data + updAttrInfo.offset) = *(float *)value;
                        break;
                    case STRING:
                        strcpy(data + updAttrInfo.offset, (char *)value);
                        break;
                }
            }
            TRY(fileHandle.UpdateRec(record));
            VLOG(1) << "update end";
        }
    }
    TRY(scan.CloseScan());
    TRY(pRmm->CloseFile(fileHandle));

    std::cout << cnt << " tuple(s) updated." << std::endl;

    return 0;
}
