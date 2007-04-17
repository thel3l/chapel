// Block LU with pivoting.
// Magic square of dimension 10, is used as test matrix A.

// Using array alias operators to implement LU block algorithm
// with pivoting.
// In this version, A1D is a range, not a 1D domain.  The iterator,
// IterateByBlocks returns ranges, not subdomains of A1D.  Temporary
// ranges, slice1 and slice2 are used to define what should be
// subdomains sliced by indefinite ranges.  
//
// The maxIndex reduction routine and the swap operator are not
// implemented yet and need to be coded.  Since passing array aliases
// with slicing to functions doesn't seem to be fully supported yet,
// the pivot calculation and the swapping of pivot rows is coded inline
// in the algorithm.

param n = 10;
param blk = 5;

var A1D = 1..n;
var slice1, slice2: range;
const A2D = [A1D,A1D]; 
var A: [A2D] real;
var piv: [A1D] int;
var ind, temp:int;
var rtemp: real;

initA(A,'Adata.dat');
[i in A1D] piv(i) = i;

writeln("Unfactored Matrix:");
writeln(A);

for (UnfactoredInds,CurrentBlockInds,TrailingBlockInds) 
  in IterateByBlocks(A1D,blk) {
  
  var A1 => A[UnfactoredInds,CurrentBlockInds];
  var A2 => A[UnfactoredInds,TrailingBlockInds];
  var A12 => A[CurrentBlockInds,TrailingBlockInds];
  var A22 => A[TrailingBlockInds,TrailingBlockInds];

// LU factorization of A1 
  for k in CurrentBlockInds {
//  temporaries used instead of subdomains with indefinite ranges.
    slice1 = k+1..UnfactoredInds.high;
    slice2 = k+1..CurrentBlockInds.high;

//  ind = maxIndex reduce (A1(UnfactoredInds,k));
    var maxAk = abs(A1(k,k));
    ind = k;
    for i in slice1 {
      if (abs(A1(i,k)) > maxAk) {
        ind = i;
        maxAk = abs(A1(i,k));    
      }
    }
    if (ind != k) {
//      piv(k) <==> piv(ind);
//      A(k,..) <==> A(ind,..);
      temp = piv(k);
      piv(k) = piv(ind);
      piv(ind) = temp;
      for i in A1D {
        rtemp = A(k,i);
        A(k,i) = A(ind,i);
        A(ind,i) = rtemp;       
      } 
    } 
    if (A1(k,k) != 0.0) {
      forall i in slice1 {
        A1(i,k) = A1(i,k)/A1(k,k);
      }
//  This conditional test is needed.  Otherwise the array
//  access goes out of bounds.
      if (slice2.high >= slice2.low) then
      forall (i,j) in [slice1,slice2] {
        A1(i,j) -= A1(i,k)*A1(k,j);
      }
    } 
    else halt("zero pivot encountered");
  }

// Update of A12.
  forall j in TrailingBlockInds {
    for k in CurrentBlockInds {
//  temporary range used instead of subdomain with indefinite range
      slice2 = k+1..CurrentBlockInds.high;
      forall i in slice2 {
        A12(i,j) -= A1(i,k)*A12(k,j);
      }
    }
  }
// Update of A22 -= A12*A21.
  forall (i,j) in [TrailingBlockInds, TrailingBlockInds] {
    for k in CurrentBlockInds {
      A22(i,j) -= A1(i,k)*A12(k,j);
    }
  }
}
writeln();
writeln("Factored Matrix:");
writeln(A);
writeln();
writeln("Pivot Vector:");
writeln(piv);

iterator IterateByBlocks(D:range,blksize) {
  var n = D.length;
  var lo,hi: int;

  for i in D by blksize {
    lo = i;
    hi = min(i + blksize-1,n);
    yield (lo..n,i..hi,hi+1..n); 
  }   
}
  
def initA(A,filename:string){

  var Adat = file(filename,path='./',mode='r');

  Adat.open;
  for ij in A.domain {
    Adat.read(A(ij));
  }
  Adat.close;
}
