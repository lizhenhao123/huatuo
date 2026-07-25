// Copyright 2026 The HuaTuo Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package service

import (
	"reflect"
	"testing"

	"huatuo-bamai/internal/storage/driver"
)

func TestBuildProfileAggregationQueryPreservesTargetMatchers(t *testing.T) {
	query := buildProfileAggregationQuery(&SearchFilter{
		ContainerID:       "containerd://4df60fc5",
		ContainerHostname: "checkout-api-7b9f6d8c4f-k2x7m",
	})
	want := []driver.Filter{
		{
			Field: profileFieldContainerID + ".keyword",
			Op:    driver.OpEq,
			Value: "containerd://4df60fc5",
		},
		{
			Field: profileFieldContainerHostname + ".keyword",
			Op:    driver.OpEq,
			Value: "checkout-api-7b9f6d8c4f-k2x7m",
		},
	}

	if !reflect.DeepEqual(query.Filters, want) {
		t.Fatalf("query filters = %#v, want %#v", query.Filters, want)
	}
}

func TestBuildProfileAggregationQueryAddsTracerIDOnce(t *testing.T) {
	query := buildProfileAggregationQuery(&SearchFilter{TracerID: "task-20260722-8f6a"})
	want := []driver.Filter{
		{
			Field: profileFieldTracerID,
			Op:    driver.OpEq,
			Value: "task-20260722-8f6a",
		},
	}

	if !reflect.DeepEqual(query.Filters, want) {
		t.Fatalf("query filters = %#v, want %#v", query.Filters, want)
	}
}

func TestProfileAggregationQueryKeepsHostOnlySemantics(t *testing.T) {
	query := buildProfileAggregationQuery(&SearchFilter{Hostname: "node-a"})
	want := []driver.Filter{
		{
			Field: profileFieldHostname + ".keyword",
			Op:    driver.OpEq,
			Value: "node-a",
		},
		{
			Field: profileFieldContainerHostname,
			Op:    driver.OpEq,
			Value: "",
		},
	}

	if !reflect.DeepEqual(query.Filters, want) {
		t.Fatalf("query filters = %#v, want %#v", query.Filters, want)
	}
}

func TestProfileAggregationQueryCombinesHostAndContainer(t *testing.T) {
	query := buildProfileAggregationQuery(&SearchFilter{
		Hostname:          "node-a",
		ContainerHostname: "worker",
	})
	want := []driver.Filter{
		{
			Field: profileFieldHostname + ".keyword",
			Op:    driver.OpEq,
			Value: "node-a",
		},
		{
			Field: profileFieldContainerHostname + ".keyword",
			Op:    driver.OpEq,
			Value: "worker",
		},
	}

	if !reflect.DeepEqual(query.Filters, want) {
		t.Fatalf("query filters = %#v, want %#v", query.Filters, want)
	}
}

func TestProfileAggregationQueryCanIncludeContainerCandidatesForHost(t *testing.T) {
	query := buildProfileAggregationQuery(&SearchFilter{
		Hostname:                 "node-a",
		IncludeContainerProfiles: true,
	})
	want := []driver.Filter{
		{
			Field: profileFieldHostname + ".keyword",
			Op:    driver.OpEq,
			Value: "node-a",
		},
	}

	if !reflect.DeepEqual(query.Filters, want) {
		t.Fatalf("query filters = %#v, want %#v", query.Filters, want)
	}
}
